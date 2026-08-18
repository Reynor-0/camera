#include "capture_session.hpp"
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
#include <thread>
#include <vector>

#if !defined(CAMERA_DEMO_VERSION)
#define CAMERA_DEMO_VERSION "unknown"
#endif

#if defined(CAMERA_DEMO_TARGET_RK3568_AARCH64)
static_assert(sizeof(void*) == 8U,
              "RK3568 AArch64 build requires a 64-bit target compiler");
#endif

namespace {

/** 收到的第一个停止信号；0 表示尚未请求停止。 */
volatile std::sig_atomic_t g_stop_signal = 0;

/** @brief 记录第一个停止信号，让主控制流进入正常清理路径。 */
void requestStop(int signal_number)
{
    if (g_stop_signal == 0) {
        g_stop_signal = signal_number;
    }
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
    /** true 表示忽略 duration_seconds，持续运行到收到停止信号。 */
    bool run_forever{false};

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

    /**
     * 诊断模式下要人为触发的采集超时恢复次数；0 表示不注入故障。
     *
     * 每次注入会在至少 10 帧正常显示后制造一组连续超时，只用于验证恢复状态机，
     * 不改变 V4L2 驱动或硬件状态。
     */
    std::uint32_t injected_capture_recoveries{0U};

    /** 诊断模式下人为让 L1 stream recovery 失败的次数；0 表示不注入。 */
    std::uint32_t injected_stream_recovery_failures{0U};
};

/** 连续多少次采集超时后执行一次 L1 stream recovery。 */
const std::uint32_t kCaptureTimeoutRecoveryThreshold = 3U;

/** 每次等待 V4L2 帧的上限，单位毫秒。 */
const int kCapturePollTimeoutMilliseconds = 1000;

/** 一个恢复预算窗口内最多允许的 L1 stream recovery 次数。 */
const std::size_t kCaptureRecoveryBudgetLimit = 3U;

/** 恢复预算滑动窗口长度，单位秒。 */
const std::uint32_t kCaptureRecoveryBudgetWindowSeconds = 60U;

/** 60 秒窗口内最多允许的 L2 V4L2 session rebuild 次数。 */
const std::size_t kCaptureSessionRecoveryBudgetLimit = 2U;

/** L2 session rebuild 的滑动预算窗口，单位秒。 */
const std::uint32_t kCaptureSessionRecoveryBudgetWindowSeconds = 60U;

/** 两次诊断故障注入之间至少正常显示的帧数。 */
const std::uint64_t kDiagnosticFramesBetweenRecoveries = 10U;

/** @brief 同步 worker 对外稳定返回的进程退出码。 */
enum class WorkerExitCode {
    Success = 0,
    RuntimeFailure = 1,
    Usage = 2,
    Configuration = 10,
    Capture = 20,
    Transform = 30,
    Display = 40,
    Internal = 50,
};

/** @brief 当前异常发生时正在操作的 pipeline 故障域。 */
enum class FailureDomain {
    Configuration,
    Capture,
    Transform,
    Display,
    Internal,
};

/** @brief 长期 worker 的顶层生命周期状态。 */
enum class PipelineState {
    Starting,
    Running,
    Stopping,
    Stopped,
    Failed,
};

/** @brief 正常离开连续显示循环的原因。 */
enum class StopReason {
    None,
    DurationElapsed,
    SigInt,
    SigTerm,
};

/**
 * @brief 为顶层 main() 保留故障域和稳定退出码。
 *
 * 底层对象仍使用标准异常报告 Linux/RGA/DRM 错误；runStream() 在资源已经完成
 * RAII 回滚后，将异常包装为本类型，供 supervisor 区分故障域。
 */
class PipelineFailure : public std::runtime_error {
public:
    /**
     * @brief 创建带故障域和稳定退出码的异常。
     * @param domain 异常发生时正在执行的 pipeline 操作域。
     * @param code supervisor 可据此决定是否重启的进程退出码。
     * @param detail 底层异常的完整可读信息。
     */
    PipelineFailure(FailureDomain domain,
                    WorkerExitCode code,
                    const std::string& detail)
        : std::runtime_error(detail), domain_(domain), code_(code)
    {
    }

    /** @brief 返回异常所属的 pipeline 故障域。 */
    FailureDomain domain() const noexcept { return domain_; }

    /** @brief 返回应由 main() 使用的稳定进程退出码。 */
    WorkerExitCode exitCode() const noexcept { return code_; }

private:
    FailureDomain domain_;
    WorkerExitCode code_;
};

/** @brief 表示某一级局部恢复已达到时间窗口预算，调用方不得继续升级同一次尝试。 */
class RecoveryBudgetExhausted : public std::runtime_error {
public:
    /** @param detail 包含恢复级别、次数和窗口的诊断文本。 */
    explicit RecoveryBudgetExhausted(const std::string& detail)
        : std::runtime_error(detail)
    {
    }
};

/**
 * @brief 管理同步 camera worker 的顶层生命周期和正常停止原因。
 *
 * 本对象不拥有 V4L2、RGA 或 DRM 资源，只管理 Starting -> Running -> Stopping ->
 * Stopped 状态。对象只在主线程访问，不是线程安全类型。
 */
class PipelineController {
public:
    /** @brief 创建处于 Starting 状态的控制器。 */
    PipelineController() = default;

    /**
     * @brief 在全部设备初始化且 STREAMON 成功后进入 Running。
     * @param run_forever true 表示没有时间截止点，只响应停止信号。
     * @param duration_seconds 定时模式的最大运行秒数；forever 模式忽略该值。
     * @throws std::logic_error 当前状态不是 Starting。
     */
    void beginRunning(bool run_forever, std::uint32_t duration_seconds)
    {
        if (state_ != PipelineState::Starting) {
            throw std::logic_error(
                "PipelineController::beginRunning requires Starting");
        }
        run_forever_ = run_forever;
        stream_start_ = std::chrono::steady_clock::now();
        deadline_ = stream_start_ + std::chrono::seconds(duration_seconds);
        state_ = PipelineState::Running;
    }

    /**
     * @brief 检查停止信号和定时截止点，决定是否继续处理新帧。
     * @return 仍处于 Running 时返回 true；已进入 Stopping 时返回 false。
     * @throws std::logic_error 当前状态既不是 Running 也不是 Stopping。
     */
    bool shouldContinue()
    {
        if (state_ == PipelineState::Stopping) {
            return false;
        }
        if (state_ != PipelineState::Running) {
            throw std::logic_error(
                "PipelineController::shouldContinue requires Running");
        }

        if (g_stop_signal == SIGINT) {
            requestStop(StopReason::SigInt);
        } else if (g_stop_signal == SIGTERM) {
            requestStop(StopReason::SigTerm);
        } else if (!run_forever_ &&
                   std::chrono::steady_clock::now() >= deadline_) {
            requestStop(StopReason::DurationElapsed);
        }
        return state_ == PipelineState::Running;
    }

    /**
     * @brief 在全部显式资源清理成功后进入 Stopped。
     * @throws std::logic_error 当前状态不是 Stopping。
     */
    void finish()
    {
        if (state_ != PipelineState::Stopping) {
            throw std::logic_error(
                "PipelineController::finish requires Stopping");
        }
        state_ = PipelineState::Stopped;
    }

    /** @brief 在异常离开 pipeline 时记录 Failed，不执行任何资源操作。 */
    void fail() noexcept { state_ = PipelineState::Failed; }

    /** @brief 返回当前生命周期状态。 */
    PipelineState state() const noexcept { return state_; }

    /** @brief 返回正常停止原因；未进入 Stopping 前为 None。 */
    StopReason stopReason() const noexcept { return stop_reason_; }

    /** @brief 返回 STREAMON 后记录的单调时钟起点。 */
    std::chrono::steady_clock::time_point streamStart() const noexcept
    {
        return stream_start_;
    }

private:
    /**
     * @brief 从 Running 原子地进入 Stopping 并固定第一个停止原因。
     * @param reason 非 None 的正常停止原因。
     */
    void requestStop(StopReason reason)
    {
        if (state_ == PipelineState::Running && reason != StopReason::None) {
            stop_reason_ = reason;
            state_ = PipelineState::Stopping;
        }
    }

    PipelineState state_{PipelineState::Starting};
    StopReason stop_reason_{StopReason::None};
    bool run_forever_{false};
    std::chrono::steady_clock::time_point stream_start_{};
    std::chrono::steady_clock::time_point deadline_{};
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

    /** 实际开始执行的 L1 capture stream recovery 次数。 */
    std::uint64_t capture_recovery_attempts{0U};

    /** 完成 STREAMOFF/QBUF/STREAMON 的 L1 recovery 次数。 */
    std::uint64_t capture_recovery_successes{0U};

    /** 已开始但任一步骤失败的 L1 recovery 次数。 */
    std::uint64_t capture_recovery_failures{0U};

    /** 因 60 秒恢复预算耗尽而拒绝执行的恢复次数。 */
    std::uint64_t capture_recovery_budget_exhaustions{0U};

    /** 全部成功 L1 recovery 的累计耗时，单位毫秒。 */
    std::uint64_t capture_recovery_total_milliseconds{0U};

    /** 实际开始执行的 L2 capture session rebuild 次数。 */
    std::uint64_t capture_session_recovery_attempts{0U};

    /** 成功重新 open、分配 buffer 并 STREAMON 的 L2 次数。 */
    std::uint64_t capture_session_recovery_successes{0U};

    /** 已开始但新 session 创建失败的 L2 次数。 */
    std::uint64_t capture_session_recovery_failures{0U};

    /** 因 L2 时间窗口预算耗尽而拒绝 rebuild 的次数。 */
    std::uint64_t capture_session_recovery_budget_exhaustions{0U};

    /** 全部成功 L2 session rebuild 的累计耗时，单位毫秒。 */
    std::uint64_t capture_session_recovery_total_milliseconds{0U};

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

    /** true 表示 stream recovery 后的下一帧应建立新的 sequence 连续性基线。 */
    bool sequence_baseline_pending{false};

    /** 第一帧 RGA 返回的版本字符串。 */
    std::string rga_version;
};

/**
 * @brief 限制滑动时间窗口内允许发起的某一级恢复次数。
 *
 * 本对象只记录恢复尝试的单调时钟时间，不拥有 V4L2 资源。超过预算时拒绝本次恢复，
 * 由顶层按对应故障域退出，避免永久硬件故障造成无界恢复忙循环。
 */
class RecoveryBudget {
public:
    /**
     * @brief 创建一个滑动窗口恢复预算。
     * @param limit 窗口内允许的最大恢复次数，必须大于 0。
     * @param window_seconds 滑动窗口长度，单位秒，必须大于 0。
     * @throws std::invalid_argument 参数为 0 时抛出。
     */
    RecoveryBudget(std::size_t limit, std::uint32_t window_seconds)
        : limit_(limit), window_seconds_(window_seconds)
    {
        if (limit_ == 0U || window_seconds_ == 0U) {
            throw std::invalid_argument(
                "RecoveryBudget requires a non-zero limit and window");
        }
    }

    /**
     * @brief 尝试消费一次恢复预算。
     * @return 当前窗口尚未达到 limit 时记录本次尝试并返回 true；否则返回 false。
     */
    bool tryAcquire()
    {
        const std::chrono::steady_clock::time_point now =
            std::chrono::steady_clock::now();
        const std::chrono::seconds window(window_seconds_);
        while (!attempts_.empty() && now - attempts_.front() >= window) {
            attempts_.erase(attempts_.begin());
        }
        if (attempts_.size() >= limit_) {
            return false;
        }
        attempts_.push_back(now);
        return true;
    }

    /** @brief 返回当前滑动窗口内已经消费的恢复次数。 */
    std::size_t attemptsInWindow() const noexcept { return attempts_.size(); }

    /** @brief 返回窗口内最多允许的恢复次数。 */
    std::size_t limit() const noexcept { return limit_; }

    /** @brief 返回滑动窗口长度，单位秒。 */
    std::uint32_t windowSeconds() const noexcept { return window_seconds_; }

private:
    std::size_t limit_;
    std::uint32_t window_seconds_;
    std::vector<std::chrono::steady_clock::time_point> attempts_;
};

/** @brief 返回用于日志的稳定生命周期状态名。 */
const char* pipelineStateName(PipelineState state)
{
    switch (state) {
        case PipelineState::Starting:
            return "STARTING";
        case PipelineState::Running:
            return "RUNNING";
        case PipelineState::Stopping:
            return "STOPPING";
        case PipelineState::Stopped:
            return "STOPPED";
        case PipelineState::Failed:
            return "FAILED";
    }
    return "UNKNOWN";
}

/** @brief 返回用于日志和 supervisor 诊断的稳定停止原因名。 */
const char* stopReasonName(StopReason reason)
{
    switch (reason) {
        case StopReason::None:
            return "none";
        case StopReason::DurationElapsed:
            return "duration-elapsed";
        case StopReason::SigInt:
            return "SIGINT";
        case StopReason::SigTerm:
            return "SIGTERM";
    }
    return "unknown";
}

/** @brief 返回用于日志的稳定故障域名称。 */
const char* failureDomainName(FailureDomain domain)
{
    switch (domain) {
        case FailureDomain::Configuration:
            return "configuration";
        case FailureDomain::Capture:
            return "capture";
        case FailureDomain::Transform:
            return "transform";
        case FailureDomain::Display:
            return "display";
        case FailureDomain::Internal:
            return "internal";
    }
    return "unknown";
}

/** @brief 将故障域映射为本阶段规定的稳定退出码。 */
WorkerExitCode failureExitCode(FailureDomain domain)
{
    switch (domain) {
        case FailureDomain::Configuration:
            return WorkerExitCode::Configuration;
        case FailureDomain::Capture:
            return WorkerExitCode::Capture;
        case FailureDomain::Transform:
            return WorkerExitCode::Transform;
        case FailureDomain::Display:
            return WorkerExitCode::Display;
        case FailureDomain::Internal:
            return WorkerExitCode::Internal;
    }
    return WorkerExitCode::RuntimeFailure;
}

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
           " [--color-mode MODE] [diagnostic-options]"
           " [video-device] [drm-device]\n"
        << "  " << program
        << " --run-forever --confirm-desktop-stopped"
           " [--color-mode MODE] [diagnostic-options]"
           " [video-device] [drm-device]\n"
        << "  " << program << " -h | --help\n"
        << "  " << program << " --version\n\n"
        << "The program keeps four V4L2 MMAP capture buffers streaming and\n"
        << "alternates two XRGB8888 DRM framebuffers. RGA synchronously reads\n"
        << "each exported capture DMA-BUF, rotates 270 degrees, and writes only\n"
        << "the framebuffer not currently scanned by the CRTC. --run-forever\n"
        << "keeps the worker active until SIGINT or SIGTERM; both signals follow\n"
        << "the normal cleanup path and return exit code 0. Three consecutive\n"
        << "1-second capture timeouts trigger STREAMOFF/QBUF/STREAMON recovery.\n"
        << "At most three recoveries are allowed in a 60-second window.\n\n"
        << "Color modes:\n"
        << "  auto            trust V4L2 metadata; reject unsupported BT.709 full\n"
        << "  bt601-limited   force RGA BT.601 limited conversion (diagnostic)\n"
        << "  bt601-full      force RGA BT.601 full conversion (diagnostic)\n"
        << "  bt709-limited   force RGA BT.709 limited conversion (diagnostic)\n\n"
        << "Diagnostics:\n"
        << "  --inject-capture-timeout-recoveries N\n"
        << "      inject 1..10 synthetic timeout streaks after normal frames;\n"
        << "      this verifies recovery control flow without changing hardware.\n"
        << "  --inject-stream-recovery-failures N\n"
        << "      fail 1..10 L1 attempts before STREAMOFF to exercise L2 session\n"
        << "      rebuild; use with capture timeout recovery injection.\n\n"
        << "Example:\n"
        << "  " << program
        << " --stream 10 --confirm-desktop-stopped --color-mode"
           " bt709-limited /dev/video0 /dev/dri/card0\n";
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
 * @brief 解析 1..10 范围的诊断故障注入次数。
 * @param text 只允许包含十进制数字的参数文本。
 * @param option_name 用于错误信息的命令行选项名。
 * @return 1..10 范围内的故障注入次数。
 * @throws std::invalid_argument 文本或范围无效时抛出。
 */
std::uint32_t parseDiagnosticCount(const std::string& text,
                                   const std::string& option_name)
{
    if (text.empty()) {
        throw std::invalid_argument(option_name + " must not be empty");
    }
    for (std::size_t index = 0U; index < text.size(); ++index) {
        if (text[index] < '0' || text[index] > '9') {
            throw std::invalid_argument(
                option_name + " must contain decimal digits only");
        }
    }

    std::size_t consumed = 0U;
    unsigned long value = 0UL;
    try {
        value = std::stoul(text, &consumed, 10);
    } catch (const std::exception&) {
        throw std::invalid_argument(option_name + " must be between 1 and 10");
    }
    if (consumed != text.size() || value < 1UL || value > 10UL) {
        throw std::invalid_argument(option_name + " must be between 1 and 10");
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
    if (argc < 3) {
        throw std::invalid_argument(
            "expected --stream <seconds> or --run-forever");
    }

    Options options;
    int next_argument = 0;
    const std::string run_mode = argv[1];
    if (run_mode == "--stream") {
        if (argc < 4 ||
            std::string(argv[3]) != "--confirm-desktop-stopped") {
            throw std::invalid_argument(
                "expected --stream <seconds> --confirm-desktop-stopped");
        }
        options.duration_seconds = parseDuration(argv[2]);
        next_argument = 4;
    } else if (run_mode == "--run-forever") {
        if (std::string(argv[2]) != "--confirm-desktop-stopped") {
            throw std::invalid_argument(
                "expected --run-forever --confirm-desktop-stopped");
        }
        options.run_forever = true;
        next_argument = 3;
    } else {
        throw std::invalid_argument(
            "expected --stream <seconds> or --run-forever");
    }

    bool color_mode_seen = false;
    bool recovery_injection_seen = false;
    bool stream_failure_injection_seen = false;
    bool have_video_device = false;
    bool have_drm_device = false;
    for (int index = next_argument; index < argc; ++index) {
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
        } else if (argument == "--inject-capture-timeout-recoveries") {
            if (recovery_injection_seen || index + 1 >= argc) {
                throw std::invalid_argument(
                    "--inject-capture-timeout-recoveries requires one value "
                    "and may appear only once");
            }
            recovery_injection_seen = true;
            options.injected_capture_recoveries =
                parseDiagnosticCount(
                    argv[++index],
                    "--inject-capture-timeout-recoveries");
        } else if (argument == "--inject-stream-recovery-failures") {
            if (stream_failure_injection_seen || index + 1 >= argc) {
                throw std::invalid_argument(
                    "--inject-stream-recovery-failures requires one value "
                    "and may appear only once");
            }
            stream_failure_injection_seen = true;
            options.injected_stream_recovery_failures =
                parseDiagnosticCount(
                    argv[++index],
                    "--inject-stream-recovery-failures");
        } else if (argument.size() >= 2U &&
                   argument[0U] == '-' && argument[1U] == '-') {
            throw std::invalid_argument("unknown option: " + argument);
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
    } else if (statistics->sequence_baseline_pending) {
        // 驱动可以在 STREAMOFF/STREAMON 后延续或重置 sequence。恢复后的第一帧
        // 只建立新的连续性基线，不能把合法重置误报为大量丢帧。
        statistics->last_sequence = frame.sequence;
        statistics->sequence_baseline_pending = false;
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
 * @brief 对仍然有效的 V4L2 buffer pool 执行一次 L1 stream recovery。
 *
 * 调用点必须位于采集等待超时路径，此时应用没有持有 DQBUF buffer。STREAMOFF
 * 让驱动归还并清空全部 queue ownership，随后 queueAll 和 STREAMON 复用原有 mmap
 * 与 DMA-BUF fd；DRM/RGA 资源不变，VOP 在此期间继续扫描最后一帧。
 *
 * @param queue 当前处于 Streaming 状态的 capture queue。
 * @param budget 当前进程的恢复预算，不能为空。
 * @param statistics 当前运行统计，不能为空。
 * @param inject_failure true 时在修改 queue 前注入一次诊断失败。
 * @throws std::invalid_argument 输出对象为空时抛出。
 * @throws std::runtime_error 预算耗尽，或 STREAMOFF/QBUF/STREAMON 任一步失败时
 * 抛出。
 */
void recoverCaptureStream(V4L2BufferQueue& queue,
                          RecoveryBudget* budget,
                          StreamStatistics* statistics,
                          bool inject_failure)
{
    if (budget == nullptr || statistics == nullptr) {
        throw std::invalid_argument(
            "capture recovery requires budget and statistics");
    }
    if (!budget->tryAcquire()) {
        ++statistics->capture_recovery_budget_exhaustions;
        std::cerr
            << "Recovery capture: BUDGET_EXHAUSTED level=L1 window_seconds="
            << budget->windowSeconds()
            << " limit=" << budget->limit() << '\n';
        throw RecoveryBudgetExhausted(
            "capture stream recovery budget exhausted: " +
            std::to_string(budget->limit()) +
            " attempts in " +
            std::to_string(budget->windowSeconds()) +
            " seconds");
    }

    ++statistics->capture_recovery_attempts;
    const std::uint64_t attempt = statistics->capture_recovery_attempts;
    const std::chrono::steady_clock::time_point recovery_start =
        std::chrono::steady_clock::now();
    std::cout << "Recovery capture: STARTING level=L1 attempt=" << attempt
              << " reason=consecutive-timeouts attempts_in_window="
              << budget->attemptsInWindow() << '\n';

    try {
        if (inject_failure) {
            throw std::runtime_error(
                "diagnostic injected L1 stream recovery failure");
        }
        queue.stop();
        queue.queueAll();
        queue.start();
    } catch (...) {
        ++statistics->capture_recovery_failures;
        std::cerr << "Recovery capture: FAILED level=L1 attempt=" << attempt
                  << '\n';
        throw;
    }

    const std::chrono::steady_clock::time_point recovery_end =
        std::chrono::steady_clock::now();
    const std::uint64_t elapsed_milliseconds =
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                recovery_end - recovery_start)
                .count());
    ++statistics->capture_recovery_successes;
    statistics->capture_recovery_total_milliseconds += elapsed_milliseconds;
    statistics->sequence_baseline_pending = statistics->have_sequence;
    std::cout << "Recovery capture: SUCCEEDED level=L1 attempt=" << attempt
              << " elapsed_ms=" << elapsed_milliseconds << '\n';
}

/**
 * @brief 销毁并重新创建完整 V4L2 fd、格式、buffer pool 和 streaming queue。
 *
 * L2 比 L1 成本更高，只在 L1 失败后执行。每次尝试受独立滑动窗口预算限制，并按
 * 当前窗口内第几次尝试等待 200 ms、400 ms 的短退避。CaptureSession 会先销毁旧
 * queue/fd，再创建替代 session；DRM/RGA 不变，VOP 继续扫描最后一帧。
 *
 * @param session 当前 capture session；成功后 generation 增加且 queue 已 STREAMON。
 * @param budget L2 独立恢复预算，不能为空。
 * @param statistics 当前运行统计，不能为空。
 * @param cause 触发升级恢复的 L1 或 V4L2 错误文本。
 * @throws std::invalid_argument 输出对象为空时抛出。
 * @throws std::runtime_error 预算耗尽或新 session 创建失败时抛出。
 */
void recoverCaptureSession(CaptureSession& session,
                           RecoveryBudget* budget,
                           StreamStatistics* statistics,
                           const std::string& cause)
{
    if (budget == nullptr || statistics == nullptr) {
        throw std::invalid_argument(
            "capture session recovery requires budget and statistics");
    }
    if (!budget->tryAcquire()) {
        ++statistics->capture_session_recovery_budget_exhaustions;
        std::cerr
            << "Recovery capture: BUDGET_EXHAUSTED level=L2 window_seconds="
            << budget->windowSeconds()
            << " limit=" << budget->limit() << '\n';
        throw RecoveryBudgetExhausted(
            "capture session recovery budget exhausted: " +
            std::to_string(budget->limit()) + " attempts in " +
            std::to_string(budget->windowSeconds()) + " seconds");
    }

    ++statistics->capture_session_recovery_attempts;
    const std::uint64_t attempt =
        statistics->capture_session_recovery_attempts;
    const std::uint32_t backoff_milliseconds =
        budget->attemptsInWindow() == 1U ? 200U : 400U;
    std::cout << "Recovery capture: STARTING level=L2 attempt=" << attempt
              << " cause=" << cause
              << " backoff_ms=" << backoff_milliseconds
              << " attempts_in_window=" << budget->attemptsInWindow() << '\n';

    std::this_thread::sleep_for(
        std::chrono::milliseconds(backoff_milliseconds));
    const std::chrono::steady_clock::time_point recovery_start =
        std::chrono::steady_clock::now();
    try {
        session.rebuild();
    } catch (...) {
        ++statistics->capture_session_recovery_failures;
        std::cerr << "Recovery capture: FAILED level=L2 attempt=" << attempt
                  << '\n';
        throw;
    }

    const std::chrono::steady_clock::time_point recovery_end =
        std::chrono::steady_clock::now();
    const std::uint64_t elapsed_milliseconds =
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                recovery_end - recovery_start)
                .count());
    ++statistics->capture_session_recovery_successes;
    statistics->capture_session_recovery_total_milliseconds +=
        elapsed_milliseconds;
    statistics->sequence_baseline_pending = statistics->have_sequence;
    std::cout << "Recovery capture: SUCCEEDED level=L2 attempt=" << attempt
              << " generation=" << session.generation()
              << " elapsed_ms=" << elapsed_milliseconds << '\n';
}

/**
 * @brief 验证新一代 capture session，并更新主循环使用的格式和颜色模式。
 * @param session 已成功 STREAMON 的新一代 V4L2 session。
 * @param options 用户选择的自动或强制颜色模式。
 * @param format 非空输出，成功时接收新 session 的实际格式副本。
 * @param color_mode 非空输出，成功时接收适用于新格式的 RGA CSC 模式。
 * @throws std::invalid_argument 输出指针为空时抛出。
 * @throws std::runtime_error 新 session 格式或颜色元数据不兼容时抛出。
 */
void adoptCaptureSessionFormat(const CaptureSession& session,
                               const Options& options,
                               VideoFormat* format,
                               RgaYuvToRgbMode* color_mode)
{
    if (format == nullptr || color_mode == nullptr) {
        throw std::invalid_argument(
            "capture format adoption requires non-null outputs");
    }
    const VideoFormat replacement_format = session.format();
    validateFormat(replacement_format);
    const RgaYuvToRgbMode replacement_color_mode = selectColorMode(
        replacement_format, options.force_color_mode, options.color_mode);
    *format = replacement_format;
    *color_mode = replacement_color_mode;
    std::cout << "Recovery capture: VALIDATED level=L2 generation="
              << session.generation()
              << " size=" << format->width << 'x' << format->height
              << " stride=" << format->bytes_per_line[0U]
              << " color_mode=" << colorModeName(*color_mode) << '\n';
}

/**
 * @brief 执行连续 V4L2 DMA-BUF 到双 DRM framebuffer 的同步流水线。
 * @param options 已通过命令行范围校验的配置。
 */
void runStream(const Options& options)
{
    const std::uint32_t source_width = 1920U;
    const std::uint32_t source_height = 1080U;
    PipelineController controller;
    FailureDomain failure_domain = FailureDomain::Internal;
    g_stop_signal = 0;
    std::cout << "Lifecycle: "
              << pipelineStateName(controller.state()) << '\n';

    try {
        // 尽早安装 handler，使初始化期间收到 SIGINT/SIGTERM 时也不会绕过 RAII。
        SignalHandlerGuard signal_handlers;

        failure_domain = FailureDomain::Capture;
        CaptureSessionConfig capture_config;
        capture_config.device_path = options.video_device;
        capture_config.width = source_width;
        capture_config.height = source_height;
        capture_config.pixel_format = V4L2_PIX_FMT_NV12;
        capture_config.buffer_count = 4U;
        capture_config.requested_color.colorspace = V4L2_COLORSPACE_REC709;
        capture_config.requested_color.xfer_func = V4L2_XFER_FUNC_709;
        capture_config.requested_color.ycbcr_enc = V4L2_YCBCR_ENC_709;
        capture_config.requested_color.quantization =
            V4L2_QUANTIZATION_LIM_RANGE;
        CaptureSession capture(capture_config);
        VideoFormat format = capture.format();

        failure_domain = FailureDomain::Configuration;
        validateFormat(format);
        RgaYuvToRgbMode color_mode = selectColorMode(
            format, options.force_color_mode, options.color_mode);

        failure_domain = FailureDomain::Display;
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

        failure_domain = FailureDomain::Capture;
        controller.beginRunning(options.run_forever,
                                options.duration_seconds);
        std::cout << "Lifecycle: "
                  << pipelineStateName(controller.state()) << '\n';

        StreamStatistics statistics;
        RecoveryBudget capture_recovery_budget(
            kCaptureRecoveryBudgetLimit,
            kCaptureRecoveryBudgetWindowSeconds);
        RecoveryBudget capture_session_recovery_budget(
            kCaptureSessionRecoveryBudgetLimit,
            kCaptureSessionRecoveryBudgetWindowSeconds);
        std::size_t writable_framebuffer = 0U;
        bool display_active = false;
        std::uint32_t consecutive_timeouts = 0U;
        std::uint32_t diagnostic_recoveries_completed = 0U;
        std::uint32_t diagnostic_stream_failures_completed = 0U;
        std::uint64_t next_diagnostic_injection_frame =
            kDiagnosticFramesBetweenRecoveries;
        bool diagnostic_timeout_streak_active = false;

        std::cout
            << "Continuous camera display started:\n"
            << "  Camera: " << options.video_device << '\n'
            << "  Source: 1920x1080 NV12, stride="
            << format.bytes_per_line[0U] << " bytes\n"
            << "  Capture buffers: " << capture.queue().bufferCount() << '\n'
            << "  Capture session generation: " << capture.generation() << '\n'
            << "  Color metadata: colorspace=" << format.colorspace
            << " xfer=" << format.xfer_func
            << " ycbcr=" << format.ycbcr_enc
            << " quantization=" << format.quantization << '\n'
            << "  RGA color mode: " << colorModeName(color_mode)
            << (options.force_color_mode
                    ? " (forced diagnostic)"
                    : " (metadata)")
            << '\n'
            << "  DRM framebuffers: A=" << framebuffer_a.framebufferId()
            << " B=" << framebuffer_b.framebufferId()
            << " pitch=" << framebuffer_a.pitch() << " bytes\n"
            << "  Capture recovery: "
            << kCaptureTimeoutRecoveryThreshold << " consecutive x "
            << kCapturePollTimeoutMilliseconds << " ms; limit="
            << kCaptureRecoveryBudgetLimit << " per "
            << kCaptureRecoveryBudgetWindowSeconds << " seconds\n"
            << "  Capture session recovery: limit="
            << kCaptureSessionRecoveryBudgetLimit << " per "
            << kCaptureSessionRecoveryBudgetWindowSeconds << " seconds\n"
            << "  Diagnostic recovery injections: "
            << options.injected_capture_recoveries << '\n'
            << "  Diagnostic L1 failure injections: "
            << options.injected_stream_recovery_failures << '\n'
            << "  Run limit: ";
        if (options.run_forever) {
            std::cout << "forever (until SIGINT/SIGTERM)\n";
        } else {
            std::cout << options.duration_seconds << " seconds maximum\n";
        }

        while (controller.shouldContinue()) {
            failure_domain = FailureDomain::Capture;
            if (!diagnostic_timeout_streak_active &&
                diagnostic_recoveries_completed <
                    options.injected_capture_recoveries &&
                statistics.displayed_frames >=
                    next_diagnostic_injection_frame) {
                diagnostic_timeout_streak_active = true;
                std::cout
                    << "Diagnostic injection: capture timeout streak="
                    << (diagnostic_recoveries_completed + 1U)
                    << " after_displayed_frames="
                    << statistics.displayed_frames << '\n';
            }

            bool frame_ready = false;
            try {
                frame_ready = diagnostic_timeout_streak_active
                                  ? false
                                  : capture.queue().waitForFrame(
                                        kCapturePollTimeoutMilliseconds);
            } catch (const std::exception& capture_error) {
                recoverCaptureSession(capture,
                                      &capture_session_recovery_budget,
                                      &statistics,
                                      capture_error.what());
                failure_domain = FailureDomain::Configuration;
                adoptCaptureSessionFormat(
                    capture, options, &format, &color_mode);
                failure_domain = FailureDomain::Capture;
                consecutive_timeouts = 0U;
                continue;
            }
            if (!frame_ready) {
                // poll 被停止信号打断时不把正常关机误计为采集超时。
                if (!controller.shouldContinue()) {
                    continue;
                }
                ++statistics.timeouts;
                ++consecutive_timeouts;
                if (consecutive_timeouts >=
                    kCaptureTimeoutRecoveryThreshold) {
                    const bool inject_l1_failure =
                        diagnostic_stream_failures_completed <
                        options.injected_stream_recovery_failures;
                    try {
                        recoverCaptureStream(capture.queue(),
                                             &capture_recovery_budget,
                                             &statistics,
                                             inject_l1_failure);
                    } catch (const RecoveryBudgetExhausted&) {
                        throw;
                    } catch (const std::exception& l1_error) {
                        if (inject_l1_failure) {
                            ++diagnostic_stream_failures_completed;
                        }
                        recoverCaptureSession(capture,
                                              &capture_session_recovery_budget,
                                              &statistics,
                                              l1_error.what());
                        failure_domain = FailureDomain::Configuration;
                        adoptCaptureSessionFormat(
                            capture, options, &format, &color_mode);
                        failure_domain = FailureDomain::Capture;
                    }
                    consecutive_timeouts = 0U;
                    if (diagnostic_timeout_streak_active) {
                        ++diagnostic_recoveries_completed;
                        diagnostic_timeout_streak_active = false;
                        next_diagnostic_injection_frame =
                            statistics.displayed_frames +
                            kDiagnosticFramesBetweenRecoveries;
                    }
                }
                continue;
            }

            CapturedFrame frame;
            try {
                if (!capture.queue().tryDequeue(&frame)) {
                    continue;
                }
            } catch (const std::exception& capture_error) {
                recoverCaptureSession(capture,
                                      &capture_session_recovery_budget,
                                      &statistics,
                                      capture_error.what());
                failure_domain = FailureDomain::Configuration;
                adoptCaptureSessionFormat(
                    capture, options, &format, &color_mode);
                failure_domain = FailureDomain::Capture;
                consecutive_timeouts = 0U;
                continue;
            }
            consecutive_timeouts = 0U;
            if ((frame.flags & V4L2_BUF_FLAG_ERROR) != 0U) {
                ++statistics.error_frames;
                try {
                    capture.queue().requeue(frame.buffer_index);
                } catch (const std::exception& capture_error) {
                    recoverCaptureSession(capture,
                                          &capture_session_recovery_budget,
                                          &statistics,
                                          capture_error.what());
                    failure_domain = FailureDomain::Configuration;
                    adoptCaptureSessionFormat(
                        capture, options, &format, &color_mode);
                    failure_domain = FailureDomain::Capture;
                }
                continue;
            }
            try {
                validateCapturedFrame(frame, format);
            } catch (const std::exception& capture_error) {
                // metadata 无效时当前 DQBUF buffer 的可复用性不可证明。销毁整个
                // session 比尝试 QBUF 一个布局未知的 buffer 更安全。
                recoverCaptureSession(capture,
                                      &capture_session_recovery_budget,
                                      &statistics,
                                      capture_error.what());
                failure_domain = FailureDomain::Configuration;
                adoptCaptureSessionFormat(
                    capture, options, &format, &color_mode);
                failure_domain = FailureDomain::Capture;
                continue;
            }

            failure_domain = FailureDomain::Transform;
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

            // 同步 RGA 已完成读取，capture buffer 可以立即归还 ISP；目标 DRM
            // buffer 是独立 GEM 存储，VOP 扫描不会再引用该 V4L2 buffer。
            failure_domain = FailureDomain::Internal;
            updateStatistics(frame, transform, &statistics);
            failure_domain = FailureDomain::Capture;
            try {
                capture.queue().requeue(frame.buffer_index);
            } catch (const std::exception& capture_error) {
                recoverCaptureSession(capture,
                                      &capture_session_recovery_budget,
                                      &statistics,
                                      capture_error.what());
                failure_domain = FailureDomain::Configuration;
                adoptCaptureSessionFormat(
                    capture, options, &format, &color_mode);
                failure_domain = FailureDomain::Capture;
                continue;
            }

            failure_domain = FailureDomain::Display;
            if (!display_active) {
                display.show(target.framebufferId());
                display_active = true;
            } else {
                // writable_framebuffer 始终指向当前未被 VOP 扫描的目标。收到完成
                // 事件后，上一块 framebuffer 才重新成为下一轮可写目标。
                static_cast<void>(
                    display.pageFlipAndWait(target.framebufferId(), 2000));
            }
            ++statistics.displayed_frames;
            writable_framebuffer = 1U - writable_framebuffer;
        }

        const std::chrono::steady_clock::time_point stream_end =
            std::chrono::steady_clock::now();
        std::cout << "Lifecycle: "
                  << pipelineStateName(controller.state())
                  << " reason=" << stopReasonName(controller.stopReason())
                  << '\n';

        // 定时运行却始终没有帧说明链路不健康；初始化期间立即收到停止信号则属于
        // 合法的受控退出，仍需走下面的显式清理路径。
        if ((!display_active || statistics.displayed_frames == 0U) &&
            controller.stopReason() == StopReason::DurationElapsed) {
            failure_domain = FailureDomain::Capture;
            throw std::runtime_error("no camera frame reached the display");
        }

        failure_domain = FailureDomain::Capture;
        capture.queue().stop();
        const std::uint64_t completed_flips = display.completedFlipCount();

        failure_domain = FailureDomain::Display;
        const DrmCrtcRestoreResult restore_result = display.restore();
        framebuffer_b.release();
        framebuffer_a.release();

        failure_domain = FailureDomain::Internal;
        controller.finish();

        const double elapsed_seconds =
            std::chrono::duration_cast<std::chrono::duration<double> >(
                stream_end - controller.streamStart())
                .count();
        const double capture_fps =
            elapsed_seconds > 0.0
                ? static_cast<double>(statistics.captured_frames) /
                      elapsed_seconds
                : 0.0;
        const double display_fps =
            elapsed_seconds > 0.0
                ? static_cast<double>(statistics.displayed_frames) /
                      elapsed_seconds
                : 0.0;
        const double average_rga_microseconds =
            statistics.captured_frames > 0U
                ? static_cast<double>(statistics.rga_total_microseconds) /
                      static_cast<double>(statistics.captured_frames)
                : 0.0;

        std::cout
            << "Continuous camera display complete:\n"
            << "  Stop reason: " << stopReasonName(controller.stopReason())
            << '\n'
            << "  Captured frames: " << statistics.captured_frames << '\n'
            << "  Displayed frames: " << statistics.displayed_frames << '\n'
            << "  Completed page flips: " << completed_flips << '\n'
            << "  Error frames: " << statistics.error_frames << '\n'
            << "  Capture timeouts: " << statistics.timeouts << '\n'
            << "  Capture stream recoveries: attempted="
            << statistics.capture_recovery_attempts
            << " succeeded=" << statistics.capture_recovery_successes
            << " failed=" << statistics.capture_recovery_failures
            << " budget_exhausted="
            << statistics.capture_recovery_budget_exhaustions << '\n'
            << "  Capture recovery total ms: "
            << statistics.capture_recovery_total_milliseconds << '\n'
            << "  Capture session recoveries: attempted="
            << statistics.capture_session_recovery_attempts
            << " succeeded=" << statistics.capture_session_recovery_successes
            << " failed=" << statistics.capture_session_recovery_failures
            << " budget_exhausted="
            << statistics.capture_session_recovery_budget_exhaustions << '\n'
            << "  Capture session recovery total ms: "
            << statistics.capture_session_recovery_total_milliseconds << '\n'
            << "  Final capture session generation: "
            << capture.generation() << '\n'
            << "  Sequence gaps: " << statistics.sequence_gaps << '\n';
        if (statistics.have_sequence) {
            std::cout << "  Sequence range: " << statistics.first_sequence
                      << ".." << statistics.last_sequence << '\n';
        } else {
            std::cout << "  Sequence range: n/a\n";
        }
        std::cout
            << "  Elapsed: " << std::fixed << std::setprecision(3)
            << elapsed_seconds << " seconds\n"
            << "  Capture FPS: " << std::setprecision(2) << capture_fps << '\n'
            << "  Display FPS: " << display_fps << '\n'
            << "  RGA us: min=" << statistics.rga_min_microseconds
            << " avg=" << average_rga_microseconds
            << " max=" << statistics.rga_max_microseconds << '\n'
            << "  RGA version: "
            << (statistics.rga_version.empty() ? "n/a" : statistics.rga_version)
            << '\n'
            << "  Cleanup V4L2: STREAMOFF complete\n"
            << "  Cleanup DRM: "
            << (restore_result == DrmCrtcRestoreResult::kCrtcDisabled
                    ? "CRTC safely disabled"
                    : "no CRTC change was required")
            << '\n'
            << "  Cleanup buffers: framebuffer release complete\n"
            << "Lifecycle: " << pipelineStateName(controller.state()) << '\n';
    } catch (const PipelineFailure&) {
        controller.fail();
        throw;
    } catch (const std::exception& error) {
        controller.fail();
        std::cerr << "Lifecycle: "
                  << pipelineStateName(controller.state())
                  << " domain=" << failureDomainName(failure_domain) << '\n'
                  << "Cleanup: RAII rollback executed; external resource audit "
                     "is required\n";
        throw PipelineFailure(
            failure_domain,
            failureExitCode(failure_domain),
            std::string(failureDomainName(failure_domain)) +
                " failure: " + error.what());
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
        return static_cast<int>(WorkerExitCode::Success);
    } catch (const std::invalid_argument& error) {
        std::cerr << "camera_display_stream: " << error.what() << '\n';
        std::cerr << "Try '" << argv[0] << " --help' for usage.\n";
        return static_cast<int>(WorkerExitCode::Usage);
    } catch (const PipelineFailure& error) {
        std::cerr << "camera_display_stream: " << error.what() << '\n'
                  << "Failure domain: "
                  << failureDomainName(error.domain()) << '\n'
                  << "Exit code: "
                  << static_cast<int>(error.exitCode()) << '\n';
        return static_cast<int>(error.exitCode());
    } catch (const std::exception& error) {
        std::cerr << "camera_display_stream: " << error.what() << '\n';
        std::cerr << "Failure domain: internal\n"
                  << "Exit code: "
                  << static_cast<int>(WorkerExitCode::Internal) << '\n';
        return static_cast<int>(WorkerExitCode::Internal);
    }
}
