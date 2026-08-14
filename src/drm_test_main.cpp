#include "drm_device.hpp"
#include "drm_display.hpp"

#include <signal.h>
#include <time.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#if !defined(CAMERA_DEMO_VERSION)
// 允许开发者脱离 CMake 做临时编译，同时保持 --version 可用。
#define CAMERA_DEMO_VERSION "unknown"
#endif

#if defined(CAMERA_DEMO_TARGET_RK3568_AARCH64)
static_assert(sizeof(void*) == 8U,
              "RK3568 AArch64 build requires a 64-bit target compiler");
#endif

namespace {

/** 收到 SIGINT/SIGTERM 时由 signal handler 设置，主循环读取。 */
volatile std::sig_atomic_t g_stop_requested = 0;

/**
 * @brief 仅记录退出请求，真正的 DRM 清理仍由正常 C++ 控制流执行。
 * @param signal_number 收到的信号编号；本函数不需要区分 SIGINT 和 SIGTERM。
 */
void requestStop(int signal_number)
{
    static_cast<void>(signal_number);
    g_stop_requested = 1;
}

/** @brief 在作用域内安装 SIGINT/SIGTERM handler，并在析构时恢复。 */
class SignalHandlerGuard {
public:
    /**
     * @brief 安装只设置 sig_atomic_t 标志的信号处理函数。
     * @throws std::runtime_error 无法安装任一 handler 时抛出。
     */
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

    /** @brief 恢复进入作用域前的 signal handler。 */
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
    /** std::signal() 使用的函数指针类型。 */
    typedef void (*SignalHandler)(int);

    /** 进入作用域前的 SIGINT handler。 */
    SignalHandler old_sigint_{SIG_ERR};

    /** 进入作用域前的 SIGTERM handler。 */
    SignalHandler old_sigterm_{SIG_ERR};
};

/** @brief 保存 drm_probe 命令行参数。 */
struct Options {
    /** 要探测的 DRM primary node。 */
    std::string device_path{"/dev/dri/card0"};

    /** 是否额外创建、填充并释放一个未绑定到显示管线的 dumb framebuffer。 */
    bool test_dumb_buffer{false};

    /** 是否取得 DRM master 并把测试色条真正绑定到 CRTC。 */
    bool show_color_bars{false};

    /** 色条保持时间，单位为秒；仅 show_color_bars 为 true 时有效。 */
    std::uint32_t show_duration_seconds{0U};
};

/**
 * @brief 打印 drm_probe 命令行用法。
 * @param program argv[0] 中的程序名称。
 * @param output 接收帮助文本的输出流。
 */
void printUsage(const char* program, std::ostream& output)
{
    output << "drm_probe - DRM/KMS resource and dumb-buffer probe\n\n"
           << "Usage:\n"
           << "  " << program << " [drm-device]\n"
           << "  " << program
           << " --test-dumb-buffer [drm-device]\n"
           << "  " << program
           << " --show-color-bars <seconds> --confirm-desktop-stopped\n"
           << "      [drm-device]\n"
           << "  " << program << " -h | --help\n"
           << "  " << program << " --version\n\n"
           << "Arguments:\n"
           << "  drm-device  DRM primary node (default /dev/dri/card0)\n\n"
           << "Options:\n"
           << "  --test-dumb-buffer  create, mmap, checksum and release an\n"
           << "                      unbound mode-sized XRGB8888 framebuffer\n"
           << "  --show-color-bars   obtain DRM master and display the same\n"
           << "                      framebuffer for 1 to 300 seconds\n"
           << "  --confirm-desktop-stopped\n"
           << "                      required acknowledgement that Weston and\n"
           << "                      its clients have already been stopped\n\n"
           << "The default command is read-only. Dumb-buffer test mode creates\n"
           << "temporary unbound resources. Color-bar mode changes the CRTC and\n"
           << "requires Weston and other DRM masters to be stopped first.\n";
}

/**
 * @brief 解析并限制色条显示秒数。
 * @param text 只允许包含十进制数字的参数。
 * @return 1 到 300 范围内的秒数。
 * @throws std::invalid_argument 文本无效或数值越界时抛出。
 */
std::uint32_t parseDurationSeconds(const std::string& text)
{
    if (text.empty()) {
        throw std::invalid_argument("display duration must not be empty");
    }
    for (std::size_t index = 0U; index < text.size(); ++index) {
        if (text[index] < '0' || text[index] > '9') {
            throw std::invalid_argument(
                "display duration must contain decimal digits only");
        }
    }

    std::size_t parsed_characters = 0U;
    unsigned long seconds = 0UL;
    try {
        seconds = std::stoul(text, &parsed_characters, 10);
    } catch (const std::exception&) {
        throw std::invalid_argument(
            "display duration must be between 1 and 300 seconds");
    }
    if (parsed_characters != text.size() || seconds < 1UL || seconds > 300UL ||
        seconds >
            static_cast<unsigned long>(
                std::numeric_limits<std::uint32_t>::max())) {
        throw std::invalid_argument(
            "display duration must be between 1 and 300 seconds");
    }
    return static_cast<std::uint32_t>(seconds);
}

/**
 * @brief 解析只读 probe 和 dumb-buffer 生命周期测试参数。
 * @param argc main() 收到的参数数量。
 * @param argv main() 收到的参数数组。
 * @return 完整运行配置。
 * @throws std::invalid_argument 参数数量或选项组合无效时抛出。
 */
Options parseOptions(int argc, char* argv[])
{
    Options options;
    if (argc == 1) {
        return options;
    }

    const std::string first = argv[1];
    if (first == "--test-dumb-buffer") {
        options.test_dumb_buffer = true;
        if (argc == 3) {
            options.device_path = argv[2];
        } else if (argc != 2) {
            throw std::invalid_argument(
                "--test-dumb-buffer accepts at most one DRM device path");
        }
        return options;
    }

    if (first == "--show-color-bars") {
        if (argc != 4 && argc != 5) {
            throw std::invalid_argument(
                "--show-color-bars requires seconds and --confirm-desktop-stopped");
        }
        if (std::string(argv[3]) != "--confirm-desktop-stopped") {
            throw std::invalid_argument(
                "refusing modeset without --confirm-desktop-stopped");
        }
        options.show_color_bars = true;
        options.show_duration_seconds = parseDurationSeconds(argv[2]);
        if (argc == 5) {
            options.device_path = argv[4];
        }
        return options;
    }

    if (argc == 2 && !first.empty() && first[0] != '-') {
        options.device_path = first;
        return options;
    }
    throw std::invalid_argument("invalid drm_probe arguments");
}

/** @brief 打印构建版本和目标架构。 */
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

    std::cout << "drm_probe " << CAMERA_DEMO_VERSION
              << " (C++11, target=" << target << ")\n";
}

/**
 * @brief 打印归一化 DRM/KMS 探测结果。
 * @param device_path 本次打开的 DRM node 路径。
 * @param result DrmDevice::probe() 返回的资源选择结果。
 */
void printProbeResult(const std::string& device_path,
                      const DrmProbeResult& result)
{
    std::cout << "DRM device: " << device_path << '\n'
              << "  Driver: " << result.driver_name << '\n'
              << "  Dumb buffer: "
              << (result.dumb_buffer_supported ? "supported" : "unsupported")
              << '\n'
              << "  Connector: " << result.connector_name << '\n'
              << "  Connector ID: " << result.connector_id << '\n'
              << "  Status: connected\n"
              << "  Mode: " << result.mode.name << '\n'
              << "  Resolution: " << result.mode.width << 'x'
              << result.mode.height << '\n'
              << "  Refresh: " << std::fixed << std::setprecision(2)
              << result.mode.refresh_rate_hz << " Hz\n"
              << "  Preferred: " << (result.mode.preferred ? "yes" : "no")
              << '\n'
              << "  Encoder ID: " << result.encoder_id << '\n'
              << "  CRTC ID: " << result.crtc_id << '\n';
}

/**
 * @brief 创建、填充、校验并显式释放一个未绑定的 dumb framebuffer。
 * @param device 已打开且生命周期覆盖本函数的 DRM 设备。
 * @param mode probe() 选中的屏幕宽高，用作 framebuffer 尺寸。
 */
void runDumbBufferTest(const DrmDevice& device, const DrmModeInfo& mode)
{
    DrmDumbFramebuffer framebuffer(device.fd(), mode.width, mode.height);
    framebuffer.fillColorBars();

    std::cout << "Dumb framebuffer test:\n"
              << "  Format: XRGB8888\n"
              << "  Resolution: " << framebuffer.width() << 'x'
              << framebuffer.height() << '\n'
              << "  Pitch: " << framebuffer.pitch() << " bytes\n"
              << "  Size: " << framebuffer.size() << " bytes\n"
              << "  GEM handle: " << framebuffer.handle() << '\n'
              << "  Framebuffer ID: " << framebuffer.framebufferId() << '\n'
              << "  Checksum: 0x" << std::hex << framebuffer.checksum()
              << std::dec << '\n'
              << "  Bound to CRTC: no\n";

    framebuffer.release();
    std::cout << "  Cleanup: complete\n";
}

/**
 * @brief 等待指定时间或退出信号，不在 signal handler 中调用非安全函数。
 * @param duration_seconds 最长等待秒数，必须大于 0。
 */
void waitForDisplay(std::uint32_t duration_seconds)
{
    // 100 ms 粒度可以及时响应 Ctrl-C，同时静态 framebuffer 不需要 page-flip
    // event，因此这里不引入 DRM event loop。
    const std::uint32_t ticks_per_second = 10U;
    const std::uint32_t total_ticks = duration_seconds * ticks_per_second;
    for (std::uint32_t tick = 0U;
         tick < total_ticks && g_stop_requested == 0;
         ++tick) {
        timespec remaining{};
        remaining.tv_sec = 0;
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
 * @brief 取得 DRM master，把色条 framebuffer 显示到目标 CRTC 后安全退出。
 * @param device 已打开且生命周期覆盖本函数的 DRM 设备。
 * @param result probe() 选出的 connector、CRTC 和 mode。
 * @param duration_seconds 色条最长保持时间，单位为秒。
 */
void runColorBarDisplay(const DrmDevice& device,
                        const DrmProbeResult& result,
                        std::uint32_t duration_seconds)
{
    if (!result.dumb_buffer_supported) {
        throw std::runtime_error(
            "DRM driver does not support dumb buffers");
    }

    // 声明顺序很重要：display 后构造、先析构，保证解除 CRTC 绑定后才释放
    // framebuffer 及其 GEM 存储。
    DrmDumbFramebuffer framebuffer(device.fd(),
                                    result.mode.width,
                                    result.mode.height);
    framebuffer.fillColorBars();
    DrmCrtcDisplay display(device.fd(),
                           result.connector_id,
                           result.crtc_id,
                           result.mode.name,
                           result.mode.width,
                           result.mode.height,
                           true);

    g_stop_requested = 0;
    SignalHandlerGuard signal_handlers;
    display.show(framebuffer.framebufferId());
    std::cout << "Color bars are now visible:\n"
              << "  Framebuffer ID: " << framebuffer.framebufferId() << '\n'
              << "  CRTC ID: " << result.crtc_id << '\n'
              << "  Connector ID: " << result.connector_id << '\n'
              << "  Duration: " << duration_seconds << " seconds maximum\n"
              << "  Press Ctrl-C to stop early.\n";

    waitForDisplay(duration_seconds);
    const DrmCrtcRestoreResult restore_result = display.restore();
    framebuffer.release();

    if (restore_result == DrmCrtcRestoreResult::kCrtcDisabled) {
        std::cout << "Display cleanup: CRTC safely disabled; restart Weston\n";
    } else {
        std::cout << "Display cleanup: no CRTC change was required\n";
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
                printVersion();
                return EXIT_SUCCESS;
            }
        }

        const Options options = parseOptions(argc, argv);
        const DrmDevice device(options.device_path);
        const DrmProbeResult result = device.probe();
        printProbeResult(device.path(), result);
        if (options.test_dumb_buffer) {
            runDumbBufferTest(device, result.mode);
        } else if (options.show_color_bars) {
            runColorBarDisplay(device,
                               result,
                               options.show_duration_seconds);
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "drm_probe: " << error.what() << '\n';
        std::cerr << "Try '" << argv[0] << " --help' for usage.\n";
        return EXIT_FAILURE;
    }
}
