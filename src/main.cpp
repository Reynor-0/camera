#include "v4l2_buffer.hpp"
#include "v4l2_device.hpp"

#include <linux/videodev2.h>

#include <csignal>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#if !defined(CAMERA_DEMO_VERSION)
// 非 CMake 构建仍能给出明确版本，而不是因为缺少编译定义而失败。
#define CAMERA_DEMO_VERSION "unknown"
#endif

#if defined(CAMERA_DEMO_TARGET_RK3568_AARCH64)
// RK3568 的默认构建目标是 64 位 AArch64 Linux。误用 32 位工具链时在编译阶段
// 直接失败，避免部署后才发现 ELF ABI 与 rootfs 不匹配。
static_assert(sizeof(void*) == 8U,
              "RK3568 AArch64 build requires a 64-bit target compiler");
#endif

namespace {

/** SIGINT/SIGTERM handler 只修改 sig_atomic_t，满足异步信号安全要求。 */
volatile std::sig_atomic_t g_stop_requested = 0;

/**
 * V4L2 FOURCC 的最高 bit 表示 big-endian 格式。部分较旧 BSP 的 videodev2.h 没有
 * 暴露 V4L2_PIX_FMT_FLAG_BE 名称，因此在仅用于显示的代码中保留兼容常量。
 */
const std::uint32_t kFourccBigEndianFlag = 1U << 31U;

/**
 * @brief 请求采集循环在完成当前系统调用后退出。
 *
 * @param signal_number 触发 handler 的信号编号；本函数不需要区分具体编号。
 */
void requestStop(int signal_number)
{
    static_cast<void>(signal_number);
    g_stop_requested = 1;
}

/** @brief 保存 `--capture` 模式的命令行参数。 */
struct CaptureOptions {
    /** V4L2 capture node。 */
    std::string device{"/dev/video0"};

    /** 请求宽度，单位为像素。 */
    std::uint32_t width{640U};

    /** 请求高度，单位为像素。 */
    std::uint32_t height{360U};

    /** 请求的 V4L2 fourcc。 */
    std::uint32_t pixel_format{V4L2_PIX_FMT_NV12};

    /** 向 VIDIOC_REQBUFS 请求的 buffer 数量。 */
    std::uint32_t buffer_count{4U};

    /** 正常退出前需要成功 DQBUF 的帧数。 */
    std::uint32_t frame_count{100U};

    /** 每次 poll 等待帧的最长时间，单位为毫秒。 */
    int timeout_ms{2000};

    /** 是否在开始采集前将全部 MMAP memory planes 导出为 DMA-BUF fd。 */
    bool export_dma_buffers{false};
};

/**
 * @brief 将 V4L2 32-bit fourcc 转换为四字符字符串。
 *
 * @param value V4L2_PIX_FMT_* 或 v4l2_fmtdesc.pixelformat 数值。
 * @return 长度固定为 4 的字符串；big-endian flag 不在本阶段单独格式化。
 */
std::string fourccToString(std::uint32_t value)
{
    // big-endian flag 占据最高 bit，不属于 FOURCC 的第四个字符。
    value &= ~kFourccBigEndianFlag;
    std::string result(4, ' ');
    result[0] = static_cast<char>(value & 0xffU);
    result[1] = static_cast<char>((value >> 8U) & 0xffU);
    result[2] = static_cast<char>((value >> 16U) & 0xffU);
    result[3] = static_cast<char>((value >> 24U) & 0xffU);
    return result;
}

/**
 * @brief 将四字符命令行参数编码为 V4L2 fourcc。
 *
 * @param text 用户输入的格式，例如 `NV12` 或 `YUYV`。
 * @return 可传给 VIDIOC_S_FMT 的 fourcc 数值。
 * @throws std::invalid_argument 输入不是四个字符时抛出。
 */
std::uint32_t parseFourcc(const std::string& text)
{
    if (text.size() != 4U) {
        throw std::invalid_argument("FOURCC must contain exactly four characters");
    }
    return v4l2_fourcc(text[0], text[1], text[2], text[3]);
}

/**
 * @brief 将十进制参数转换为 uint32_t，并拒绝尾随字符和溢出。
 *
 * @param text 命令行中的十进制字符串。
 * @param option_name 参数名称，用于错误信息。
 * @return 经过范围检查的 uint32_t。
 * @throws std::invalid_argument 字符串无效、含尾随字符或超出 uint32_t 时抛出。
 */
std::uint32_t parseUint32(const std::string& text,
                          const std::string& option_name)
{
    if (text.empty() || text[0U] == '-') {
        throw std::invalid_argument(option_name + " requires an unsigned integer");
    }
    std::size_t consumed = 0U;
    unsigned long value = 0UL;
    try {
        value = std::stoul(text, &consumed, 10);
    } catch (const std::exception&) {
        throw std::invalid_argument(option_name + " requires an unsigned integer");
    }

    if (consumed != text.size() ||
        value > static_cast<unsigned long>(
                    std::numeric_limits<std::uint32_t>::max())) {
        throw std::invalid_argument(option_name + " is outside the uint32 range");
    }
    return static_cast<std::uint32_t>(value);
}

/**
 * @brief 解析 `--capture` 后的设备路径和 key/value 参数。
 *
 * @param argc main() 收到的参数数量。
 * @param argv main() 收到的参数数组。
 * @return 完整采集配置；未指定选项使用 CaptureOptions 默认值。
 * @throws std::invalid_argument 缺少 value、选项未知或数值范围无效时抛出。
 */
CaptureOptions parseCaptureOptions(int argc, char* argv[])
{
    if (argc < 3) {
        throw std::invalid_argument("--capture requires a video device path");
    }

    CaptureOptions options;
    options.device = argv[2];

    for (int index = 3; index < argc;) {
        const std::string name = argv[index];
        if (name == "--export-dmabuf") {
            options.export_dma_buffers = true;
            ++index;
            continue;
        }

        if (index + 1 >= argc) {
            throw std::invalid_argument(name + " requires a value");
        }

        const std::string value = argv[index + 1];
        if (name == "--width") {
            options.width = parseUint32(value, name);
        } else if (name == "--height") {
            options.height = parseUint32(value, name);
        } else if (name == "--format") {
            options.pixel_format = parseFourcc(value);
        } else if (name == "--buffers") {
            options.buffer_count = parseUint32(value, name);
        } else if (name == "--frames") {
            options.frame_count = parseUint32(value, name);
        } else if (name == "--timeout-ms") {
            const std::uint32_t parsed = parseUint32(value, name);
            if (parsed >
                static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
                throw std::invalid_argument(name + " exceeds the int range");
            }
            options.timeout_ms = static_cast<int>(parsed);
        } else {
            throw std::invalid_argument("unknown capture option: " + name);
        }
        index += 2;
    }

    if (options.width == 0U || options.height == 0U ||
        options.buffer_count == 0U || options.frame_count == 0U) {
        throw std::invalid_argument(
            "width, height, buffers and frames must be greater than zero");
    }
    return options;
}

/**
 * @brief 将一种离散尺寸或尺寸范围打印到标准输出。
 * @param size VIDIOC_ENUM_FRAMESIZES 的归一化结果。
 */
void printFrameSize(const FrameSizeInfo& size)
{
    if (size.type == FrameSizeInfo::Type::Discrete) {
        std::cout << "      " << size.min_width << 'x' << size.min_height
                  << '\n';
        return;
    }

    const char* name = size.type == FrameSizeInfo::Type::Continuous
                           ? "continuous"
                           : "stepwise";
    std::cout << "      " << name << ": " << size.min_width << 'x'
              << size.min_height << " .. " << size.max_width << 'x'
              << size.max_height;
    if (size.type == FrameSizeInfo::Type::Stepwise) {
        std::cout << " step " << size.step_width << 'x' << size.step_height;
    }
    std::cout << '\n';
}

/**
 * @brief 打印设备身份和应用实际使用的 capture API。
 * @param device V4L2 设备节点路径。
 * @param capabilities VIDIOC_QUERYCAP 的归一化结果。
 */
void printCapabilities(const std::string& device,
                       const DeviceCapabilities& capabilities)
{
    std::cout << "Device: " << device << '\n'
              << "  Driver: " << capabilities.driver << '\n'
              << "  Card: " << capabilities.card << '\n'
              << "  Bus: " << capabilities.bus_info << '\n'
              << "  Capture API: "
              << (capabilities.capture_type ==
                          V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                      ? "multi-planar"
                      : "single-planar")
              << '\n'
              << "  Streaming: yes\n"
              << "  Capabilities: 0x" << std::hex
              << capabilities.effective_caps << std::dec << '\n';
}

/**
 * @brief 打印驱动最终接受的视频格式和每个 memory plane 的布局。
 * @param requested_width 用户请求宽度。
 * @param requested_height 用户请求高度。
 * @param requested_fourcc 用户请求 fourcc。
 * @param actual VIDIOC_G_FMT 返回的实际格式。
 */
void printActualFormat(std::uint32_t requested_width,
                       std::uint32_t requested_height,
                       std::uint32_t requested_fourcc,
                       const VideoFormat& actual)
{
    std::cout << "Requested format: " << requested_width << 'x'
              << requested_height << ' ' << fourccToString(requested_fourcc)
              << '\n'
              << "Actual format:    " << actual.width << 'x' << actual.height
              << ' ' << fourccToString(actual.pixel_format) << '\n'
              << "Memory planes:    " << actual.plane_count << '\n'
              << "Color metadata:   colorspace=" << actual.colorspace
              << " xfer=" << actual.xfer_func
              << " ycbcr=" << actual.ycbcr_enc
              << " quantization=" << actual.quantization << '\n';
    for (std::uint32_t plane = 0U; plane < actual.plane_count; ++plane) {
        std::cout << "  Plane " << plane
                  << ": stride=" << actual.bytes_per_line[plane]
                  << ", size=" << actual.size_image[plane] << '\n';
    }
}

/**
 * @brief 执行原有设备探测模式，并可选设置一种格式。
 * @param argc main() 参数数量。
 * @param argv main() 参数数组。
 * @return 成功时返回 EXIT_SUCCESS。
 */
int runProbe(int argc, char* argv[])
{
    const std::string device = argc >= 2 ? argv[1] : "/dev/video0";
    V4L2Device camera(device);
    printCapabilities(device, camera.queryCapabilities());

    const std::vector<PixelFormatInfo> formats = camera.enumerateFormats();
    std::cout << "\nFormats (" << formats.size() << "):\n";
    for (const PixelFormatInfo& format : formats) {
        std::cout << "  " << fourccToString(format.pixel_format) << " ("
                  << format.description << ')';
        if ((format.pixel_format & kFourccBigEndianFlag) != 0U) {
            std::cout << " [big-endian]";
        }
        if ((format.flags & V4L2_FMT_FLAG_COMPRESSED) != 0U) {
            std::cout << " [compressed]";
        }
        if ((format.flags & V4L2_FMT_FLAG_EMULATED) != 0U) {
            std::cout << " [emulated]";
        }
        std::cout << '\n';

        if (format.frame_sizes.empty()) {
            std::cout << "      frame sizes not enumerated by driver\n";
        } else {
            for (const FrameSizeInfo& size : format.frame_sizes) {
                printFrameSize(size);
            }
        }
    }

    if (argc == 5) {
        const std::uint32_t width = parseUint32(argv[2], "width");
        const std::uint32_t height = parseUint32(argv[3], "height");
        const std::uint32_t pixel_format = parseFourcc(argv[4]);
        const VideoFormat actual =
            camera.setFormat(width, height, pixel_format);
        std::cout << '\n';
        printActualFormat(width, height, pixel_format, actual);
    }
    return EXIT_SUCCESS;
}

/**
 * @brief 执行 MMAP 连续采集并输出每帧 metadata。
 * @param options 已通过范围校验的采集配置。
 * @return 完成指定帧数或收到退出信号时返回 EXIT_SUCCESS。
 */
int runCapture(const CaptureOptions& options)
{
    V4L2Device camera(options.device);
    printCapabilities(options.device, camera.queryCapabilities());

    const VideoFormat actual = camera.setFormat(options.width,
                                                options.height,
                                                options.pixel_format);
    std::cout << '\n';
    printActualFormat(options.width,
                      options.height,
                      options.pixel_format,
                      actual);

    V4L2BufferQueue queue(camera, actual);
    queue.requestBuffers(options.buffer_count);
    std::cout << "Buffers: requested=" << options.buffer_count
              << ", actual=" << queue.bufferCount() << '\n';
    if (options.export_dma_buffers) {
        queue.exportDmaBuffers();
        std::cout << "DMA-BUF exports:\n";
        for (std::size_t buffer = 0U; buffer < queue.bufferCount(); ++buffer) {
            for (std::uint32_t plane = 0U; plane < queue.planeCount(); ++plane) {
                std::cout << "  buffer=" << buffer
                          << " plane=" << plane
                          << " fd=" << queue.dmaBufFd(
                                 static_cast<std::uint32_t>(buffer), plane)
                          << '\n';
            }
        }
    }
    queue.queueAll();
    queue.start();

    std::uint32_t captured_count = 0U;
    std::uint32_t error_count = 0U;
    std::uint32_t timeout_count = 0U;
    std::uint32_t sequence_gap_count = 0U;
    std::uint32_t previous_sequence = 0U;
    bool have_previous_sequence = false;
    std::uint32_t consecutive_timeouts = 0U;

    while (captured_count < options.frame_count && g_stop_requested == 0) {
        if (!queue.waitForFrame(options.timeout_ms)) {
            if (g_stop_requested != 0) {
                break;
            }
            ++timeout_count;
            ++consecutive_timeouts;
            if (consecutive_timeouts >= 3U) {
                throw std::runtime_error(
                    "capture timed out three consecutive times");
            }
            continue;
        }

        CapturedFrame frame;
        if (!queue.tryDequeue(&frame)) {
            continue;
        }
        consecutive_timeouts = 0U;

        if ((frame.flags & V4L2_BUF_FLAG_ERROR) != 0U) {
            ++error_count;
        }
        if (have_previous_sequence &&
            frame.sequence != previous_sequence + 1U) {
            ++sequence_gap_count;
        }
        previous_sequence = frame.sequence;
        have_previous_sequence = true;

        std::cout << "frame=" << captured_count
                  << " buffer=" << frame.buffer_index
                  << " sequence=" << frame.sequence
                  << " timestamp=" << frame.timestamp.tv_sec << '.'
                  << std::setw(6) << std::setfill('0')
                  << frame.timestamp.tv_usec << std::setfill(' ')
                  << " flags=0x" << std::hex << frame.flags << std::dec
                  << " planes=" << frame.planes.size() << '\n';
        for (std::size_t plane = 0U; plane < frame.planes.size(); ++plane) {
            std::cout << "  plane=" << plane
                      << " bytesused=" << frame.planes[plane].bytes_used
                      << " data_offset=" << frame.planes[plane].data_offset
                      << " mapped=" << frame.planes[plane].mapped_length
                      << " dma_buf_fd=" << frame.planes[plane].dma_buf_fd
                      << '\n';
        }

        // 本阶段只读取 metadata，不保留帧。QBUF 成功后 frame.data 指针即失效。
        queue.requeue(frame.buffer_index);
        ++captured_count;
    }

    queue.stop();
    std::cout << "Capture complete: captured=" << captured_count
              << " errors=" << error_count
              << " timeouts=" << timeout_count
              << " sequence_gaps=" << sequence_gap_count << '\n';
    return EXIT_SUCCESS;
}

/**
 * @brief 将程序支持的 probe、set-format 和 capture 帮助写入指定输出流。
 *
 * @param program argv[0] 中的程序名称。
 * @param output 接收帮助文本的输出流；`--help` 使用 stdout，参数错误使用 stderr。
 */
void printUsage(const char* program, std::ostream& output)
{
    output
        << "camera_demo - V4L2 capability probe and MMAP capture utility\n\n"
        << "Usage:\n"
        << "  " << program << " [video-device]\n"
        << "  " << program << " [video-device] <width> <height> <FOURCC>\n"
        << "  " << program << " --capture <video-device> [options]\n"
        << "  " << program << " -h | --help\n"
        << "  " << program << " --version\n\n"
        << "Modes:\n"
        << "  probe             enumerate capabilities, formats and frame sizes\n"
        << "  set format        request a format and print the driver's actual format\n"
        << "  --capture         capture frames through V4L2 MMAP streaming I/O\n\n"
        << "Capture options:\n"
        << "  --width N       requested width (default 640)\n"
        << "  --height N      requested height (default 360)\n"
        << "  --format FOURCC requested format (default NV12)\n"
        << "  --buffers N     requested MMAP buffers (default 4)\n"
        << "  --frames N      frames to capture (default 100)\n"
        << "  --timeout-ms N  poll timeout (default 2000)\n"
        << "  --export-dmabuf export every MMAP memory plane before capture\n\n"
        << "Examples:\n"
        << "  " << program << " /dev/video0\n"
        << "  " << program << " /dev/video0 1920 1080 NV12\n"
        << "  " << program
        << " --capture /dev/video0 --width 640 --height 360"
           " --format NV12 --frames 100 --export-dmabuf\n";
}

/**
 * @brief 打印构建版本和目标架构信息。
 *
 * 输出保持单行，便于部署脚本保存和比较。target 字段描述当前可执行文件的编译
 * 目标，不依赖运行时探测设备。
 */
void printVersion()
{
#if defined(CAMERA_DEMO_TARGET_RK3568_AARCH64)
    const char* target = "rk3568-aarch64-linux";
#elif defined(__x86_64__)
    const char* target = "x86_64-linux";
#elif defined(__aarch64__)
    const char* target = "aarch64-linux";
#else
    const char* target = "unknown-linux";
#endif

    std::cout << "camera_demo " << CAMERA_DEMO_VERSION
              << " (C++11, target=" << target << ")\n";
}

}  // namespace

int main(int argc, char* argv[])
{
    try {
        // 自描述选项必须在任何设备访问之前处理。用户只有可执行文件时，可以在
        // 没有摄像头权限、甚至没有 /dev/video* 的机器上获取帮助和版本。
        if (argc == 2) {
            const std::string command = argv[1];
            if (command == "-h" || command == "--help") {
                printUsage(argv[0], std::cout);
                return EXIT_SUCCESS;
            }
            if (command == "--version") {
                printVersion();
                return EXIT_SUCCESS;
            }
        }

        if (argc >= 2 && std::string(argv[1]) == "--capture") {
            // signal() 属于 ISO C/C++11，并足以让单线程 poll 循环响应 Ctrl+C。
            std::signal(SIGINT, requestStop);
            std::signal(SIGTERM, requestStop);
            return runCapture(parseCaptureOptions(argc, argv));
        }

        if (argc != 1 && argc != 2 && argc != 5) {
            printUsage(argv[0], std::cerr);
            return EXIT_FAILURE;
        }
        return runProbe(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "camera_demo: " << error.what() << '\n';
        std::cerr << "Try '" << argv[0] << " --help' for usage.\n";
        return EXIT_FAILURE;
    }
}
