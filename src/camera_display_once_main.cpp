#include "drm_device.hpp"
#include "drm_display.hpp"
#include "rga_transform.hpp"
#include "v4l2_buffer.hpp"
#include "v4l2_device.hpp"

#include <linux/videodev2.h>

#include <signal.h>
#include <time.h>

#include <cerrno>
#include <array>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#if !defined(CAMERA_DEMO_VERSION)
#define CAMERA_DEMO_VERSION "unknown"
#endif

#if defined(CAMERA_DEMO_TARGET_RK3568_AARCH64)
static_assert(sizeof(void*) == 8U,
              "RK3568 AArch64 build requires a 64-bit target compiler");
#endif

namespace {

volatile std::sig_atomic_t g_stop_requested = 0;

/** @brief 请求单帧测试尽快进入正常清理路径。 */
void requestStop(int signal_number)
{
    static_cast<void>(signal_number);
    g_stop_requested = 1;
}

/** @brief 在当前作用域安装并恢复 SIGINT/SIGTERM handler。 */
class SignalHandlerGuard {
public:
    SignalHandlerGuard()
    {
        old_sigint_ = std::signal(SIGINT, requestStop);
        if (old_sigint_ == SIG_ERR) {
            throw std::runtime_error("failed to install SIGINT handler");
        }
        old_sigterm_ = std::signal(SIGTERM, requestStop);
        if (old_sigterm_ == SIG_ERR) {
            static_cast<void>(std::signal(SIGINT, old_sigint_));
            old_sigint_ = SIG_ERR;
            throw std::runtime_error("failed to install SIGTERM handler");
        }
    }

    ~SignalHandlerGuard()
    {
        if (old_sigterm_ != SIG_ERR) {
            static_cast<void>(std::signal(SIGTERM, old_sigterm_));
        }
        if (old_sigint_ != SIG_ERR) {
            static_cast<void>(std::signal(SIGINT, old_sigint_));
        }
    }

    SignalHandlerGuard(const SignalHandlerGuard&) = delete;
    SignalHandlerGuard& operator=(const SignalHandlerGuard&) = delete;

private:
    typedef void (*SignalHandler)(int);
    SignalHandler old_sigint_{SIG_ERR};
    SignalHandler old_sigterm_{SIG_ERR};
};

/** @brief 单帧端到端测试的命令行配置。 */
struct Options {
    /** 目标帧保持在屏幕上的秒数。 */
    std::uint32_t duration_seconds{10U};

    /** V4L2 capture node。 */
    std::string video_device{"/dev/video0"};

    /** DRM primary node。 */
    std::string drm_device{"/dev/dri/card0"};

    /** true 表示忽略驱动颜色元数据，使用用户指定的诊断转换模式。 */
    bool force_color_mode{false};

    /** 自动选择失败时可由命令行强制指定的 RGA CSC 模式。 */
    RgaYuvToRgbMode color_mode{RgaYuvToRgbMode::Bt709Limited};
};

/** @brief 保存一帧有效 Y 像素区域的分布摘要。 */
struct LumaStatistics {
    /** 参与统计的有效像素数，不包含 stride 尾部填充。 */
    std::uint64_t sample_count{0U};

    /** 本帧出现的最小和最大 Y 值。 */
    std::uint8_t minimum{0U};
    std::uint8_t maximum{0U};

    /** Y 值平均数。 */
    double mean{0.0};

    /** 1%、50% 和 99% 分位值。 */
    std::uint8_t percentile_1{0U};
    std::uint8_t percentile_50{0U};
    std::uint8_t percentile_99{0U};

    /** 位于 limited-range 名义范围之外的样本数量。 */
    std::uint64_t below_16{0U};
    std::uint64_t above_235{0U};
};

/**
 * @brief 输出程序用法。
 * @param program argv[0] 中的程序名。
 * @param output 接收帮助的输出流。
 */
void printUsage(const char* program, std::ostream& output)
{
    output
        << "camera_display_once - one captured DMA-BUF through RGA to DRM\n\n"
        << "Usage:\n"
        << "  " << program
        << " --show <seconds> --confirm-desktop-stopped"
           " [--color-mode MODE] [video-device] [drm-device]\n"
        << "  " << program << " -h | --help\n"
        << "  " << program << " --version\n\n"
        << "The program requests 1920x1080 NV12 BT.709 limited range,\n"
        << "captures one V4L2 MMAP buffer, passes its exported DMA-BUF\n"
        << "to RGA, rotates/converts into a 1080x1920 DRM framebuffer,\n"
        << "requeues the capture buffer, and displays the copied result.\n\n"
        << "Color modes:\n"
        << "  auto            trust V4L2 metadata; reject unsupported BT.709 full\n"
        << "  bt601-limited   force RGA BT.601 limited conversion (diagnostic)\n"
        << "  bt601-full      force RGA BT.601 full conversion (diagnostic)\n"
        << "  bt709-limited   force RGA BT.709 limited conversion (diagnostic)\n\n"
        << "The installed librga has no BT.709 full conversion mode. Every run\n"
        << "prints active-image luma statistics; use controlled black and white\n"
        << "scenes to distinguish limited-range from full-range samples.\n";
}

/**
 * @brief 把命令行颜色模式转换为板载 RGA 支持的枚举。
 * @param text `bt601-limited`、`bt601-full` 或 `bt709-limited`。
 * @return 与命令行文本对应的 RGA CSC 模式。
 * @throws std::invalid_argument 文本不是板载 RGA 支持的模式时抛出。
 */
RgaYuvToRgbMode parseColorMode(const std::string& text)
{
    if (text == "bt601-limited") {
        return RgaYuvToRgbMode::Bt601Limited;
    }
    if (text == "bt601-full") {
        return RgaYuvToRgbMode::Bt601Full;
    }
    if (text == "bt709-limited") {
        return RgaYuvToRgbMode::Bt709Limited;
    }
    throw std::invalid_argument(
        "color mode must be auto, bt601-limited, bt601-full, or bt709-limited");
}

/**
 * @brief 返回用于诊断日志的稳定 RGA CSC 模式名称。
 * @param mode 已通过参数解析或元数据选择的模式。
 * @return 静态字符串，生命周期覆盖整个进程。
 */
const char* colorModeName(RgaYuvToRgbMode mode)
{
    switch (mode) {
        case RgaYuvToRgbMode::Bt601Limited:
            return "BT.601 limited";
        case RgaYuvToRgbMode::Bt601Full:
            return "BT.601 full";
        case RgaYuvToRgbMode::Bt709Limited:
            return "BT.709 limited";
    }
    return "unknown";
}

/**
 * @brief 解析 1..300 秒的十进制时长。
 * @param text 命令行文本。
 * @return 经过范围校验的秒数。
 * @throws std::invalid_argument 文本或范围无效时抛出。
 */
std::uint32_t parseDuration(const std::string& text)
{
    if (text.empty()) {
        throw std::invalid_argument("duration must not be empty");
    }
    for (std::size_t index = 0U; index < text.size(); ++index) {
        if (text[index] < '0' || text[index] > '9') {
            throw std::invalid_argument(
                "duration must contain decimal digits only");
        }
    }

    std::size_t consumed = 0U;
    unsigned long value = 0UL;
    try {
        value = std::stoul(text, &consumed, 10);
    } catch (const std::exception&) {
        throw std::invalid_argument("duration must be between 1 and 300");
    }
    if (consumed != text.size() || value < 1UL || value > 300UL ||
        value > static_cast<unsigned long>(
                    std::numeric_limits<std::uint32_t>::max())) {
        throw std::invalid_argument("duration must be between 1 and 300");
    }
    return static_cast<std::uint32_t>(value);
}

/**
 * @brief 解析独占显示测试参数。
 * @param argc main() 收到的参数数量。
 * @param argv main() 收到的参数数组。
 * @return 完整测试配置。
 * @throws std::invalid_argument 缺少显式桌面停止确认或参数无效时抛出。
 */
Options parseOptions(int argc, char* argv[])
{
    if (argc < 4 || std::string(argv[1]) != "--show" ||
        std::string(argv[3]) != "--confirm-desktop-stopped") {
        throw std::invalid_argument(
            "expected --show <seconds> --confirm-desktop-stopped");
    }

    Options options;
    options.duration_seconds = parseDuration(argv[2]);
    bool color_mode_seen = false;
    bool have_video_device = false;
    bool have_drm_device = false;
    for (int index = 4; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--color-mode") {
            if (color_mode_seen || index + 1 >= argc) {
                throw std::invalid_argument(
                    "--color-mode requires one value and may appear only once");
            }
            color_mode_seen = true;
            const std::string value = argv[++index];
            if (value == "auto") {
                options.force_color_mode = false;
            } else {
                options.force_color_mode = true;
                options.color_mode = parseColorMode(value);
            }
        } else if (argument == "--allow-bt601-full-fallback") {
            if (color_mode_seen) {
                throw std::invalid_argument(
                    "only one color mode option may be specified");
            }
            color_mode_seen = true;
            options.force_color_mode = true;
            options.color_mode = RgaYuvToRgbMode::Bt601Full;
        } else if (!have_video_device) {
            options.video_device = argument;
            have_video_device = true;
        } else if (!have_drm_device) {
            options.drm_device = argument;
            have_drm_device = true;
        } else {
            throw std::invalid_argument("too many device paths");
        }
    }
    return options;
}

/**
 * @brief 验证 ISP 布局并为实际量化范围选择 RGA 转换模式。
 * @param format VIDIOC_G_FMT 回填的实际格式。
 * @param force_color_mode true 时忽略颜色元数据并返回 requested_mode。
 * @param requested_mode 用户明确指定的板载 RGA CSC 诊断模式。
 * @return 与实际量化范围兼容、且经调用方明确授权的 RGA 模式。
 * @throws std::runtime_error 格式、布局或颜色元数据不兼容时抛出。
 */
RgaYuvToRgbMode selectColorMode(const VideoFormat& format,
                                bool force_color_mode,
                                RgaYuvToRgbMode requested_mode)
{
    if (format.width != 1920U || format.height != 1080U ||
        format.pixel_format != V4L2_PIX_FMT_NV12) {
        throw std::runtime_error(
            "camera did not accept 1920x1080 NV12");
    }
    if (format.plane_count != 1U || format.bytes_per_line[0U] < format.width ||
        (format.bytes_per_line[0U] % 2U) != 0U) {
        throw std::runtime_error(
            "this stage requires one-memory-plane NV12 with an even stride");
    }

    const std::uint64_t required_size =
        static_cast<std::uint64_t>(format.bytes_per_line[0U]) *
        (format.height + format.height / 2U);
    if (required_size > format.size_image[0U]) {
        throw std::runtime_error(
            "camera NV12 sizeimage is smaller than stride * 1.5 height");
    }

    if (force_color_mode) {
        return requested_mode;
    }

    const bool rec709_colorspace =
        format.colorspace == V4L2_COLORSPACE_REC709 ||
        format.colorspace == V4L2_COLORSPACE_DEFAULT;
    const bool rec709_encoding =
        format.ycbcr_enc == V4L2_YCBCR_ENC_709 ||
        format.ycbcr_enc == V4L2_YCBCR_ENC_DEFAULT;
    if (!rec709_colorspace || !rec709_encoding) {
        throw std::runtime_error(
            "camera output is not BT.709 YCbCr as required by the RGA job");
    }
    if (format.quantization == V4L2_QUANTIZATION_LIM_RANGE) {
        return RgaYuvToRgbMode::Bt709Limited;
    }
    if (format.quantization == V4L2_QUANTIZATION_FULL_RANGE) {
        throw std::runtime_error(
            "camera rejected BT.709 limited range; installed librga cannot "
            "correctly convert BT.709 full range (use an explicit "
            "--color-mode diagnostic override to compare supported modes)");
    }
    throw std::runtime_error(
        "camera returned unsupported V4L2 quantization=" +
        std::to_string(format.quantization));
}

/**
 * @brief 在 256 桶直方图中查找指定百分比分位值。
 * @param histogram 一帧有效 Y 像素的 8-bit 直方图。
 * @param sample_count histogram 中的总样本数，必须大于 0。
 * @param percentage 0 到 100 的整数百分比。
 * @return 第一个使累计样本超过目标位置的 Y 值。
 */
std::uint8_t lumaPercentile(
    const std::array<std::uint64_t, 256U>& histogram,
    std::uint64_t sample_count,
    std::uint32_t percentage)
{
    const std::uint64_t target =
        ((sample_count - 1U) * static_cast<std::uint64_t>(percentage)) / 100U;
    std::uint64_t cumulative = 0U;
    for (std::size_t value = 0U; value < histogram.size(); ++value) {
        cumulative += histogram[value];
        if (cumulative > target) {
            return static_cast<std::uint8_t>(value);
        }
    }
    return 255U;
}

/**
 * @brief 统计 NV12 有效 Y 图像区域，不读取 stride 尾部和 UV 区域。
 * @param frame 当前由应用持有、包含一个可读 MMAP memory plane 的帧。
 * @param format 驱动返回的 NV12 宽、高、stride 和 sizeimage。
 * @return 最小值、最大值、平均值、分位值和 limited-range 越界计数。
 * @throws std::runtime_error mmap 视图不足以覆盖有效 Y 图像区域时抛出。
 */
LumaStatistics analyzeLuma(const CapturedFrame& frame,
                           const VideoFormat& format)
{
    if (frame.planes.size() != 1U || frame.planes[0U].data == nullptr) {
        throw std::runtime_error("luma analysis requires one mapped plane");
    }
    const CapturedPlane& plane = frame.planes[0U];
    const std::uint64_t required_bytes =
        static_cast<std::uint64_t>(plane.data_offset) +
        static_cast<std::uint64_t>(format.bytes_per_line[0U]) * format.height;
    if (required_bytes > plane.mapped_length ||
        required_bytes > plane.bytes_used) {
        throw std::runtime_error(
            "mapped capture plane does not cover the active luma image");
    }

    std::array<std::uint64_t, 256U> histogram{};
    const std::uint8_t* const base =
        static_cast<const std::uint8_t*>(plane.data) + plane.data_offset;
    std::uint64_t sum = 0U;
    for (std::uint32_t row = 0U; row < format.height; ++row) {
        const std::uint8_t* const pixels =
            base + static_cast<std::size_t>(row) * format.bytes_per_line[0U];
        for (std::uint32_t column = 0U; column < format.width; ++column) {
            const std::uint8_t value = pixels[column];
            ++histogram[value];
            sum += value;
        }
    }

    LumaStatistics result;
    result.sample_count =
        static_cast<std::uint64_t>(format.width) * format.height;
    for (std::size_t value = 0U; value < 16U; ++value) {
        result.below_16 += histogram[value];
    }
    for (std::size_t value = 236U; value < histogram.size(); ++value) {
        result.above_235 += histogram[value];
    }
    std::size_t minimum = 0U;
    while (minimum < histogram.size() && histogram[minimum] == 0U) {
        ++minimum;
    }
    std::size_t maximum = histogram.size() - 1U;
    while (maximum > 0U && histogram[maximum] == 0U) {
        --maximum;
    }
    result.minimum = static_cast<std::uint8_t>(minimum);
    result.maximum = static_cast<std::uint8_t>(maximum);
    result.mean = static_cast<double>(sum) /
                  static_cast<double>(result.sample_count);
    result.percentile_1 =
        lumaPercentile(histogram, result.sample_count, 1U);
    result.percentile_50 =
        lumaPercentile(histogram, result.sample_count, 50U);
    result.percentile_99 =
        lumaPercentile(histogram, result.sample_count, 99U);
    return result;
}

/**
 * @brief 在比较宽松的限制下等待一帧并 DQBUF。
 * @param queue 已处于 Streaming 状态的 capture queue。
 * @return 当前由应用持有、后续必须 requeue 的帧。
 * @throws std::runtime_error 3 次超时、收到退出信号或驱动错误时抛出。
 */
CapturedFrame dequeueOneFrame(V4L2BufferQueue& queue)
{
    for (std::uint32_t attempt = 0U; attempt < 3U; ++attempt) {
        if (g_stop_requested != 0) {
            throw std::runtime_error("capture interrupted before a frame arrived");
        }
        if (!queue.waitForFrame(2000)) {
            continue;
        }
        CapturedFrame frame;
        if (queue.tryDequeue(&frame)) {
            return frame;
        }
    }
    throw std::runtime_error("capture timed out three consecutive times");
}

/**
 * @brief 保持当前 DRM framebuffer 直到超时或收到信号。
 * @param duration_seconds 最长保持时间，单位秒。
 * @throws std::runtime_error nanosleep 发生 EINTR 以外的错误时抛出。
 */
void waitForDisplay(std::uint32_t duration_seconds)
{
    const std::uint32_t total_ticks = duration_seconds * 10U;
    for (std::uint32_t tick = 0U;
         tick < total_ticks && g_stop_requested == 0;
         ++tick) {
        timespec remaining{};
        remaining.tv_nsec = 100000000L;
        while (::nanosleep(&remaining, &remaining) != 0) {
            if (errno == EINTR) {
                if (g_stop_requested != 0) {
                    return;
                }
                continue;
            }
            const int error = errno;
            throw std::runtime_error(
                "nanosleep failed: " + std::string(std::strerror(error)) +
                " (errno=" + std::to_string(error) + ")");
        }
    }
}

/**
 * @brief 执行单帧 V4L2 DMA-BUF 到 RGA/DRM 的完整所有权流程。
 * @param options 已通过命令行范围校验的配置。
 */
void runTest(const Options& options)
{
    const std::uint32_t source_width = 1920U;
    const std::uint32_t source_height = 1080U;

    V4L2Device camera(options.video_device);
    VideoColorMetadata requested_color;
    requested_color.colorspace = V4L2_COLORSPACE_REC709;
    requested_color.xfer_func = V4L2_XFER_FUNC_709;
    requested_color.ycbcr_enc = V4L2_YCBCR_ENC_709;
    requested_color.quantization = V4L2_QUANTIZATION_LIM_RANGE;
    const VideoFormat format = camera.setFormat(source_width,
                                                source_height,
                                                V4L2_PIX_FMT_NV12,
                                                requested_color);
    const RgaYuvToRgbMode color_mode = selectColorMode(
        format, options.force_color_mode, options.color_mode);

    V4L2BufferQueue queue(camera, format);
    queue.requestBuffers(4U);
    queue.exportDmaBuffers();
    queue.queueAll();
    queue.start();

    DrmDevice drm(options.drm_device);
    const DrmProbeResult display_info = drm.probe();
    if (!display_info.dumb_buffer_supported ||
        display_info.mode.width != source_height ||
        display_info.mode.height != source_width) {
        throw std::runtime_error(
            "DRM target must support dumb buffers and 1080x1920 mode");
    }
    DrmDumbFramebuffer destination(drm.fd(),
                                   display_info.mode.width,
                                   display_info.mode.height);
    if ((destination.pitch() % 4U) != 0U) {
        throw std::runtime_error(
            "XRGB8888 destination pitch is not pixel aligned");
    }
    const int destination_dma_fd = destination.dmaBufFd();

    DrmCrtcDisplay display(drm.fd(),
                           display_info.connector_id,
                           display_info.crtc_id,
                           display_info.mode.name,
                           display_info.mode.width,
                           display_info.mode.height,
                           true);
    g_stop_requested = 0;
    SignalHandlerGuard signal_handlers;

    CapturedFrame frame = dequeueOneFrame(queue);
    if ((frame.flags & V4L2_BUF_FLAG_ERROR) != 0U) {
        queue.requeue(frame.buffer_index);
        queue.stop();
        throw std::runtime_error("captured frame has V4L2_BUF_FLAG_ERROR");
    }
    if (frame.planes.size() != 1U || frame.planes[0U].dma_buf_fd < 0 ||
        frame.planes[0U].data_offset != 0U ||
        frame.planes[0U].bytes_used < format.size_image[0U]) {
        queue.requeue(frame.buffer_index);
        queue.stop();
        throw std::runtime_error(
            "captured frame has an invalid one-plane NV12 DMA-BUF layout");
    }
    const LumaStatistics luma = analyzeLuma(frame, format);

    // improcess 使用 IM_SYNC：返回时 RGA 已经不再读 capture buffer。
    // 因此可以立即 QBUF，无需等待 VOP 扫描目标 DRM buffer。
    const RgaTransformResult transform = rotateNv12ToBgrx8888(
        frame.planes[0U].dma_buf_fd,
        format.width,
        format.height,
        format.bytes_per_line[0U],
        format.height,
        destination_dma_fd,
        destination.width(),
        destination.height(),
        destination.pitch() / 4U,
        destination.height(),
        color_mode);

    const std::uint32_t captured_buffer_index = frame.buffer_index;
    const std::uint32_t captured_sequence = frame.sequence;
    const int captured_dma_fd = frame.planes[0U].dma_buf_fd;
    queue.requeue(captured_buffer_index);
    queue.stop();

    display.show(destination.framebufferId());
    std::cout
        << "Single captured frame is now visible:\n"
        << "  Camera: " << options.video_device << '\n'
        << "  Source: 1920x1080 NV12, buffer=" << captured_buffer_index
        << ", sequence=" << captured_sequence << '\n'
        << "  Source stride: " << format.bytes_per_line[0U] << " bytes\n"
        << "  Source DMA-BUF fd: " << captured_dma_fd << '\n'
        << "  Color metadata: colorspace=" << format.colorspace
        << " xfer=" << format.xfer_func
        << " ycbcr=" << format.ycbcr_enc
        << " quantization=" << format.quantization << '\n'
        << "  RGA color mode: " << colorModeName(color_mode)
        << (options.force_color_mode ? " (forced diagnostic)" : " (metadata)")
        << '\n'
        << "  Luma active pixels: count=" << luma.sample_count
        << " min=" << static_cast<unsigned int>(luma.minimum)
        << " p01=" << static_cast<unsigned int>(luma.percentile_1)
        << " p50=" << static_cast<unsigned int>(luma.percentile_50)
        << " p99=" << static_cast<unsigned int>(luma.percentile_99)
        << " max=" << static_cast<unsigned int>(luma.maximum)
        << " mean=" << std::fixed << std::setprecision(2) << luma.mean << '\n'
        << "  Luma outside limited nominal range: Y<16=" << luma.below_16
        << " (" << (100.0 * static_cast<double>(luma.below_16) /
                     static_cast<double>(luma.sample_count))
        << "%), Y>235=" << luma.above_235
        << " (" << (100.0 * static_cast<double>(luma.above_235) /
                     static_cast<double>(luma.sample_count))
        << "%)\n"
        << "  Destination: 1080x1920 XRGB8888, pitch="
        << destination.pitch() << " bytes\n"
        << "  Destination DMA-BUF fd: " << destination_dma_fd << '\n'
        << "  Rotation: 270 degrees\n"
        << "  RGA version: " << transform.version << '\n'
        << "  RGA elapsed: " << transform.elapsed_microseconds << " us\n"
        << "  Capture buffer state: requeued, then STREAMOFF completed\n"
        << "  Duration: " << options.duration_seconds
        << " seconds maximum\n";

    waitForDisplay(options.duration_seconds);
    const DrmCrtcRestoreResult restore_result = display.restore();
    destination.release();
    if (restore_result == DrmCrtcRestoreResult::kCrtcDisabled) {
        std::cout << "Cleanup: CRTC safely disabled; restart Weston\n";
    }
}

}  // namespace

int main(int argc, char* argv[])
{
    try {
        if (argc == 2) {
            const std::string command = argv[1];
            if (command == "-h" || command == "--help") {
                printUsage(argv[0], std::cout);
                return EXIT_SUCCESS;
            }
            if (command == "--version") {
                std::cout << "camera_display_once " << CAMERA_DEMO_VERSION
                          << " (C++11, target=rk3568-aarch64-linux)\n";
                return EXIT_SUCCESS;
            }
        }
        runTest(parseOptions(argc, argv));
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "camera_display_once: " << error.what() << '\n';
        std::cerr << "Try '" << argv[0] << " --help' for usage.\n";
        return EXIT_FAILURE;
    }
}
