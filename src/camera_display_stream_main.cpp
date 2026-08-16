#include "drm_device.hpp"
#include "drm_display.hpp"
#include "rga_transform.hpp"
#include "v4l2_buffer.hpp"
#include "v4l2_device.hpp"

#include <linux/videodev2.h>

#include <signal.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
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

/** @brief 请求连续显示循环进入正常清理路径。 */
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

/** @brief 连续相机显示的命令行配置。 */
struct Options {
    /** 连续显示时长，单位秒。 */
    std::uint32_t duration_seconds{10U};

    /** V4L2 capture node。 */
    std::string video_device{"/dev/video0"};

    /** DRM primary node。 */
    std::string drm_device{"/dev/dri/card0"};

    /** true 表示忽略 V4L2 颜色元数据并使用用户指定的诊断 CSC。 */
    bool force_color_mode{false};

    /** 用户指定或自动选择的 RGA YUV-to-RGB 模式。 */
    RgaYuvToRgbMode color_mode{RgaYuvToRgbMode::Bt709Limited};
};

/** @brief 保存连续 capture/RGA/page-flip 循环的运行统计。 */
struct StreamStatistics {
    /** 成功 DQBUF 且交给 RGA 的帧数。 */
    std::uint64_t captured_frames{0U};

    /** 已通过 modeset 或 flip-complete 交给屏幕的帧数。 */
    std::uint64_t displayed_frames{0U};

    /** V4L2 buffer 带 ERROR 标志而被丢弃的帧数。 */
    std::uint64_t error_frames{0U};

    /** waitForFrame() 的正常超时次数。 */
    std::uint64_t timeouts{0U};

    /** 根据 V4L2 sequence 发现的缺失帧数。 */
    std::uint64_t sequence_gaps{0U};

    /** 全部同步 RGA 作业的累计耗时，单位微秒。 */
    std::uint64_t rga_total_microseconds{0U};

    /** 单次同步 RGA 作业的最短和最长耗时，单位微秒。 */
    std::uint64_t rga_min_microseconds{0U};
    std::uint64_t rga_max_microseconds{0U};

    /** 第一帧和最近一帧的 V4L2 sequence。 */
    std::uint32_t first_sequence{0U};
    std::uint32_t last_sequence{0U};

    /** true 表示 sequence 字段已经由第一帧初始化。 */
    bool have_sequence{false};

    /** 第一帧 RGA 返回的版本字符串。 */
    std::string rga_version;
};

/**
 * @brief 输出连续显示程序的完整用法。
 * @param program argv[0] 中的程序名。
 * @param output 接收帮助文本的输出流。
 */
void printUsage(const char* program, std::ostream& output)
{
    output
        << "camera_display_stream - continuous V4L2/RGA/DRM page-flip display\n\n"
        << "Usage:\n"
        << "  " << program
        << " --stream <seconds> --confirm-desktop-stopped"
           " [--color-mode MODE] [video-device] [drm-device]\n"
        << "  " << program << " -h | --help\n"
        << "  " << program << " --version\n\n"
        << "The program keeps four V4L2 MMAP capture buffers streaming and\n"
        << "alternates two XRGB8888 DRM framebuffers. RGA synchronously reads\n"
        << "each exported capture DMA-BUF, rotates 270 degrees, and writes only\n"
        << "the framebuffer not currently scanned by the CRTC.\n\n"
        << "Color modes:\n"
        << "  auto            trust V4L2 metadata; reject unsupported BT.709 full\n"
        << "  bt601-limited   force RGA BT.601 limited conversion (diagnostic)\n"
        << "  bt601-full      force RGA BT.601 full conversion (diagnostic)\n"
        << "  bt709-limited   force RGA BT.709 limited conversion (diagnostic)\n";
}

/**
 * @brief 解析 1..3600 秒的连续显示时长。
 * @param text 只允许包含十进制数字的参数文本。
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
        throw std::invalid_argument("duration must be between 1 and 3600");
    }
    if (consumed != text.size() || value < 1UL || value > 3600UL ||
        value > static_cast<unsigned long>(
                    std::numeric_limits<std::uint32_t>::max())) {
        throw std::invalid_argument("duration must be between 1 and 3600");
    }
    return static_cast<std::uint32_t>(value);
}

/**
 * @brief 解析板载旧版 RGA 支持的强制颜色模式。
 * @param text `bt601-limited`、`bt601-full` 或 `bt709-limited`。
 * @return 对应 RGA CSC 枚举。
 * @throws std::invalid_argument 模式名无效时抛出。
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
 * @brief 返回稳定的 RGA CSC 诊断名称。
 * @param mode 已选择的转换模式。
 * @return 生命周期覆盖整个进程的静态字符串。
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
 * @brief 解析连续独占显示参数。
 * @param argc main() 收到的参数数量。
 * @param argv main() 收到的参数数组。
 * @return 完整测试配置。
 * @throws std::invalid_argument 缺少桌面停止确认或参数无效时抛出。
 */
Options parseOptions(int argc, char* argv[])
{
    if (argc < 4 || std::string(argv[1]) != "--stream" ||
        std::string(argv[3]) != "--confirm-desktop-stopped") {
        throw std::invalid_argument(
            "expected --stream <seconds> --confirm-desktop-stopped");
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
 * @brief 验证连续显示阶段要求的一内存平面 1920x1080 NV12 布局。
 * @param format VIDIOC_G_FMT 回填的实际格式。
 * @throws std::runtime_error 格式、stride 或 sizeimage 不兼容时抛出。
 */
void validateFormat(const VideoFormat& format)
{
    if (format.width != 1920U || format.height != 1080U ||
        format.pixel_format != V4L2_PIX_FMT_NV12) {
        throw std::runtime_error(
            "camera did not accept 1920x1080 NV12");
    }
    if (format.plane_count != 1U || format.bytes_per_line[0U] < format.width ||
        (format.bytes_per_line[0U] % 2U) != 0U) {
        throw std::runtime_error(
            "stream stage requires one-memory-plane NV12 with an even stride");
    }
    const std::uint64_t required_size =
        static_cast<std::uint64_t>(format.bytes_per_line[0U]) *
        (format.height + format.height / 2U);
    if (required_size > format.size_image[0U]) {
        throw std::runtime_error(
            "camera NV12 sizeimage is smaller than stride * 1.5 height");
    }
}

/**
 * @brief 根据元数据自动选择模式，或接受用户强制诊断模式。
 * @param format 已通过 NV12 布局检查的实际 V4L2 格式。
 * @param force_color_mode true 时忽略颜色元数据。
 * @param requested_mode 用户指定的板载 RGA CSC 模式。
 * @return 当前连续 RGA 作业应使用的模式。
 * @throws std::runtime_error 自动模式无法精确转换驱动元数据时抛出。
 */
RgaYuvToRgbMode selectColorMode(const VideoFormat& format,
                                bool force_color_mode,
                                RgaYuvToRgbMode requested_mode)
{
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
            "camera output is not BT.709 YCbCr as required by this stage");
    }
    if (format.quantization == V4L2_QUANTIZATION_LIM_RANGE) {
        return RgaYuvToRgbMode::Bt709Limited;
    }
    if (format.quantization == V4L2_QUANTIZATION_FULL_RANGE) {
        throw std::runtime_error(
            "installed librga cannot convert the camera's reported BT.709 full "
            "range; choose an explicit --color-mode diagnostic override");
    }
    throw std::runtime_error(
        "camera returned unsupported V4L2 quantization=" +
        std::to_string(format.quantization));
}

/**
 * @brief 验证一个 DQBUF 帧可以安全作为当前 RGA NV12 输入。
 * @param frame 当前由应用拥有且尚未 requeue 的帧。
 * @param format capture queue 的实际格式。
 * @throws std::runtime_error 帧标记或 DMA-BUF 布局无效时抛出。
 */
void validateCapturedFrame(const CapturedFrame& frame,
                           const VideoFormat& format)
{
    if ((frame.flags & V4L2_BUF_FLAG_ERROR) != 0U) {
        throw std::runtime_error("captured frame has V4L2_BUF_FLAG_ERROR");
    }
    if (frame.planes.size() != 1U || frame.planes[0U].dma_buf_fd < 0 ||
        frame.planes[0U].data_offset != 0U ||
        frame.planes[0U].bytes_used < format.size_image[0U]) {
        throw std::runtime_error(
            "captured frame has an invalid one-plane NV12 DMA-BUF layout");
    }
}

/**
 * @brief 将一帧的 sequence 和 RGA 耗时提交到累计统计。
 * @param frame 刚完成同步 RGA 读取的 V4L2 帧。
 * @param transform 对该帧执行 RGA 后返回的诊断结果。
 * @param statistics 非空的累计统计输出对象。
 */
void updateStatistics(const CapturedFrame& frame,
                      const RgaTransformResult& transform,
                      StreamStatistics* statistics)
{
    if (statistics == nullptr) {
        throw std::invalid_argument("statistics output must not be null");
    }
    if (!statistics->have_sequence) {
        statistics->first_sequence = frame.sequence;
        statistics->last_sequence = frame.sequence;
        statistics->have_sequence = true;
        statistics->rga_min_microseconds = transform.elapsed_microseconds;
        statistics->rga_version = transform.version;
    } else {
        const std::uint32_t expected = statistics->last_sequence + 1U;
        if (frame.sequence != expected) {
            statistics->sequence_gaps +=
                frame.sequence > expected
                    ? static_cast<std::uint64_t>(frame.sequence - expected)
                    : 1U;
        }
        statistics->last_sequence = frame.sequence;
        if (transform.elapsed_microseconds <
            statistics->rga_min_microseconds) {
            statistics->rga_min_microseconds = transform.elapsed_microseconds;
        }
    }
    if (transform.elapsed_microseconds > statistics->rga_max_microseconds) {
        statistics->rga_max_microseconds = transform.elapsed_microseconds;
    }
    statistics->rga_total_microseconds += transform.elapsed_microseconds;
    ++statistics->captured_frames;
}

/**
 * @brief 执行连续 V4L2 DMA-BUF 到双 DRM framebuffer 的同步流水线。
 * @param options 已通过命令行范围校验的配置。
 */
void runStream(const Options& options)
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
    validateFormat(format);
    const RgaYuvToRgbMode color_mode = selectColorMode(
        format, options.force_color_mode, options.color_mode);

    V4L2BufferQueue queue(camera, format);
    queue.requestBuffers(4U);
    queue.exportDmaBuffers();
    queue.queueAll();

    DrmDevice drm(options.drm_device);
    const DrmProbeResult display_info = drm.probe();
    if (!display_info.dumb_buffer_supported ||
        display_info.mode.width != source_height ||
        display_info.mode.height != source_width) {
        throw std::runtime_error(
            "DRM target must support dumb buffers and 1080x1920 mode");
    }

    // 声明顺序保证 display 先关闭 CRTC，然后才能删除两个 framebuffer。
    DrmDumbFramebuffer framebuffer_a(drm.fd(),
                                      display_info.mode.width,
                                      display_info.mode.height);
    DrmDumbFramebuffer framebuffer_b(drm.fd(),
                                      display_info.mode.width,
                                      display_info.mode.height);
    if ((framebuffer_a.pitch() % 4U) != 0U ||
        framebuffer_a.pitch() != framebuffer_b.pitch()) {
        throw std::runtime_error(
            "double XRGB8888 framebuffers have incompatible pitches");
    }
    DrmDumbFramebuffer* framebuffers[2] = {
        &framebuffer_a,
        &framebuffer_b,
    };
    const int destination_dma_fds[2] = {
        framebuffer_a.dmaBufFd(),
        framebuffer_b.dmaBufFd(),
    };

    DrmCrtcDisplay display(drm.fd(),
                           display_info.connector_id,
                           display_info.crtc_id,
                           display_info.mode.name,
                           display_info.mode.width,
                           display_info.mode.height,
                           true);
    g_stop_requested = 0;
    SignalHandlerGuard signal_handlers;
    queue.start();

    StreamStatistics statistics;
    std::size_t writable_framebuffer = 0U;
    bool display_active = false;
    std::uint32_t consecutive_timeouts = 0U;
    const std::chrono::steady_clock::time_point stream_start =
        std::chrono::steady_clock::now();
    const std::chrono::steady_clock::time_point deadline =
        stream_start + std::chrono::seconds(options.duration_seconds);

    std::cout
        << "Continuous camera display started:\n"
        << "  Camera: " << options.video_device << '\n'
        << "  Source: 1920x1080 NV12, stride="
        << format.bytes_per_line[0U] << " bytes\n"
        << "  Capture buffers: " << queue.bufferCount() << '\n'
        << "  Color metadata: colorspace=" << format.colorspace
        << " xfer=" << format.xfer_func
        << " ycbcr=" << format.ycbcr_enc
        << " quantization=" << format.quantization << '\n'
        << "  RGA color mode: " << colorModeName(color_mode)
        << (options.force_color_mode ? " (forced diagnostic)" : " (metadata)")
        << '\n'
        << "  DRM framebuffers: A=" << framebuffer_a.framebufferId()
        << " B=" << framebuffer_b.framebufferId()
        << " pitch=" << framebuffer_a.pitch() << " bytes\n"
        << "  Duration: " << options.duration_seconds << " seconds maximum\n";

    while (g_stop_requested == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        if (!queue.waitForFrame(1000)) {
            ++statistics.timeouts;
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
            ++statistics.error_frames;
            queue.requeue(frame.buffer_index);
            continue;
        }
        validateCapturedFrame(frame, format);

        DrmDumbFramebuffer& target = *framebuffers[writable_framebuffer];
        const RgaTransformResult transform = rotateNv12ToBgrx8888(
            frame.planes[0U].dma_buf_fd,
            format.width,
            format.height,
            format.bytes_per_line[0U],
            format.height,
            destination_dma_fds[writable_framebuffer],
            target.width(),
            target.height(),
            target.pitch() / 4U,
            target.height(),
            color_mode);

        // 同步 RGA 已完成读取，capture buffer 可以立即归还 ISP；目标 DRM buffer
        // 是独立 GEM 存储，后续 VOP 扫描不会再引用该 V4L2 buffer。
        updateStatistics(frame, transform, &statistics);
        queue.requeue(frame.buffer_index);

        if (!display_active) {
            display.show(target.framebufferId());
            display_active = true;
        } else {
            // writable_framebuffer 始终指向当前未被 VOP 扫描的目标。收到完成事件
            // 后，上一块 framebuffer 才重新成为下一轮可写目标。
            static_cast<void>(
                display.pageFlipAndWait(target.framebufferId(), 2000));
        }
        ++statistics.displayed_frames;
        writable_framebuffer = 1U - writable_framebuffer;
    }

    const std::chrono::steady_clock::time_point stream_end =
        std::chrono::steady_clock::now();
    if (!display_active || statistics.displayed_frames == 0U) {
        throw std::runtime_error("no camera frame reached the display");
    }

    queue.stop();
    const std::uint64_t completed_flips = display.completedFlipCount();
    const DrmCrtcRestoreResult restore_result = display.restore();
    framebuffer_b.release();
    framebuffer_a.release();

    const double elapsed_seconds =
        std::chrono::duration_cast<std::chrono::duration<double> >(
            stream_end - stream_start)
            .count();
    const double capture_fps =
        static_cast<double>(statistics.captured_frames) / elapsed_seconds;
    const double display_fps =
        static_cast<double>(statistics.displayed_frames) / elapsed_seconds;
    const double average_rga_microseconds =
        static_cast<double>(statistics.rga_total_microseconds) /
        static_cast<double>(statistics.captured_frames);

    std::cout
        << "Continuous camera display complete:\n"
        << "  Captured frames: " << statistics.captured_frames << '\n'
        << "  Displayed frames: " << statistics.displayed_frames << '\n'
        << "  Completed page flips: " << completed_flips << '\n'
        << "  Error frames: " << statistics.error_frames << '\n'
        << "  Capture timeouts: " << statistics.timeouts << '\n'
        << "  Sequence gaps: " << statistics.sequence_gaps << '\n'
        << "  Sequence range: " << statistics.first_sequence << ".."
        << statistics.last_sequence << '\n'
        << "  Elapsed: " << std::fixed << std::setprecision(3)
        << elapsed_seconds << " seconds\n"
        << "  Capture FPS: " << std::setprecision(2) << capture_fps << '\n'
        << "  Display FPS: " << display_fps << '\n'
        << "  RGA us: min=" << statistics.rga_min_microseconds
        << " avg=" << average_rga_microseconds
        << " max=" << statistics.rga_max_microseconds << '\n'
        << "  RGA version: " << statistics.rga_version << '\n';
    if (restore_result == DrmCrtcRestoreResult::kCrtcDisabled) {
        std::cout << "  Cleanup: CRTC safely disabled; restart Weston\n";
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
                std::cout << "camera_display_stream " << CAMERA_DEMO_VERSION
                          << " (C++11, target=rk3568-aarch64-linux)\n";
                return EXIT_SUCCESS;
            }
        }
        runStream(parseOptions(argc, argv));
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "camera_display_stream: " << error.what() << '\n';
        std::cerr << "Try '" << argv[0] << " --help' for usage.\n";
        return EXIT_FAILURE;
    }
}
