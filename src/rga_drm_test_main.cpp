#include "drm_device.hpp"
#include "drm_display.hpp"
#include "rga_transform.hpp"

#include <linux/dma-buf.h>

#include <signal.h>
#include <sys/ioctl.h>
#include <time.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <exception>
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

void requestStop(int signal_number)
{
    static_cast<void>(signal_number);
    g_stop_requested = 1;
}

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

struct Options {
    std::uint32_t duration_seconds{10U};
    std::string device_path{"/dev/dri/card0"};
};

void printUsage(const char* program, std::ostream& output)
{
    output << "rga_drm_test - offline NV12 RGA to DRM validation\n\n"
           << "Usage:\n"
           << "  " << program
           << " --show <seconds> --confirm-desktop-stopped [drm-device]\n"
           << "  " << program << " -h | --help\n"
           << "  " << program << " --version\n\n"
           << "The test creates a 1920x1080 NV12 DMA-BUF, rotates it 270\n"
           << "degrees with RGA into a 1080x1920 XRGB8888 DRM framebuffer,\n"
           << "then displays the result. Stop Weston before running it.\n";
}

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

Options parseOptions(int argc, char* argv[])
{
    if (argc != 4 && argc != 5) {
        throw std::invalid_argument(
            "--show requires seconds and --confirm-desktop-stopped");
    }
    if (std::string(argv[1]) != "--show" ||
        std::string(argv[3]) != "--confirm-desktop-stopped") {
        throw std::invalid_argument(
            "refusing RGA/DRM display without explicit desktop confirmation");
    }

    Options options;
    options.duration_seconds = parseDuration(argv[2]);
    if (argc == 5) {
        options.device_path = argv[4];
    }
    return options;
}

void dmaBufSync(int fd, std::uint64_t flags, const char* operation)
{
    dma_buf_sync sync{};
    sync.flags = flags;
    int result = 0;
    do {
        result = ::ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        const int error = errno;
        throw std::runtime_error(
            std::string(operation) + " failed: " + std::strerror(error) +
            " (errno=" + std::to_string(error) + ")");
    }
}

/** @brief 写入非对称的 BT.709 limited-range NV12 垂直色条。 */
void fillNv12ColorBars(DrmDumbBuffer& buffer,
                       std::uint32_t width,
                       std::uint32_t height,
                       std::uint32_t height_stride)
{
    if (buffer.data() == nullptr || buffer.pitch() < width ||
        height_stride < height || (width % 2U) != 0U ||
        (height % 2U) != 0U || (height_stride % 2U) != 0U) {
        throw std::invalid_argument("invalid NV12 dumb-buffer layout");
    }
    const std::uint64_t required_size =
        static_cast<std::uint64_t>(buffer.pitch()) *
        (static_cast<std::uint64_t>(height_stride) + height / 2U);
    if (required_size > buffer.size()) {
        throw std::invalid_argument(
            "NV12 image does not fit in the DRM dumb buffer");
    }

    struct YuvColor {
        unsigned char y;
        unsigned char u;
        unsigned char v;
    };
    // Red, green, blue, white and black in approximately BT.709 limited range.
    static const YuvColor kColors[] = {
        {63U, 102U, 240U},
        {173U, 42U, 26U},
        {32U, 240U, 118U},
        {235U, 128U, 128U},
        {16U, 128U, 128U},
    };
    const std::size_t color_count = sizeof(kColors) / sizeof(kColors[0]);

    unsigned char* const base = static_cast<unsigned char*>(buffer.data());
    std::memset(base, 0, buffer.size());
    for (std::uint32_t y = 0U; y < height; ++y) {
        unsigned char* const row =
            base + static_cast<std::size_t>(y) * buffer.pitch();
        for (std::uint32_t x = 0U; x < width; ++x) {
            const std::size_t color_index =
                static_cast<std::size_t>(x) * color_count / width;
            row[x] = kColors[color_index].y;
        }
    }

    unsigned char* const chroma =
        base + static_cast<std::size_t>(buffer.pitch()) * height_stride;
    for (std::uint32_t y = 0U; y < height / 2U; ++y) {
        unsigned char* const row =
            chroma + static_cast<std::size_t>(y) * buffer.pitch();
        for (std::uint32_t x = 0U; x < width; x += 2U) {
            const std::size_t color_index =
                static_cast<std::size_t>(x) * color_count / width;
            row[x] = kColors[color_index].u;
            row[x + 1U] = kColors[color_index].v;
        }
    }
}

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

void runTest(const Options& options)
{
    const std::uint32_t source_width = 1920U;
    const std::uint32_t source_height = 1080U;
    const std::uint32_t source_height_stride = source_height;
    const std::uint32_t allocation_height =
        source_height_stride + source_height / 2U;

    DrmDevice device(options.device_path);
    const DrmProbeResult display_info = device.probe();
    if (!display_info.dumb_buffer_supported) {
        throw std::runtime_error("DRM driver does not support dumb buffers");
    }
    if (display_info.mode.width != source_height ||
        display_info.mode.height != source_width) {
        throw std::runtime_error(
            "selected display mode is not the 1080x1920 rotation target");
    }

    DrmDumbBuffer source(device.fd(),
                         source_width,
                         allocation_height,
                         8U);
    const int source_dma_fd = source.dmaBufFd();
    dmaBufSync(source_dma_fd,
               DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE,
               "DMA_BUF_IOCTL_SYNC(START WRITE)");
    fillNv12ColorBars(source,
                      source_width,
                      source_height,
                      source_height_stride);
    dmaBufSync(source_dma_fd,
               DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE,
               "DMA_BUF_IOCTL_SYNC(END WRITE)");

    DrmDumbFramebuffer destination(device.fd(),
                                   display_info.mode.width,
                                   display_info.mode.height);
    const int destination_dma_fd = destination.dmaBufFd();
    if ((destination.pitch() % 4U) != 0U) {
        throw std::runtime_error(
            "XRGB8888 destination pitch is not pixel aligned");
    }

    const RgaTransformResult transform = rotateNv12ToBgrx8888(
        source_dma_fd,
        source_width,
        source_height,
        source.pitch(),
        source_height_stride,
        destination_dma_fd,
        destination.width(),
        destination.height(),
        destination.pitch() / 4U,
        destination.height());

    DrmCrtcDisplay display(device.fd(),
                           display_info.connector_id,
                           display_info.crtc_id,
                           display_info.mode.name,
                           display_info.mode.width,
                           display_info.mode.height,
                           true);
    g_stop_requested = 0;
    SignalHandlerGuard signal_handlers;
    display.show(destination.framebufferId());

    std::cout << "RGA to DRM image is now visible:\n"
              << "  Source: 1920x1080 NV12\n"
              << "  Source pitch: " << source.pitch() << " bytes\n"
              << "  Source DMA-BUF fd: " << source_dma_fd << '\n'
              << "  Destination: 1080x1920 XRGB8888\n"
              << "  Destination pitch: " << destination.pitch()
              << " bytes\n"
              << "  Destination DMA-BUF fd: " << destination_dma_fd << '\n'
              << "  Rotation: 270 degrees\n"
              << "  RGA version: " << transform.version << '\n'
              << "  RGA elapsed: " << transform.elapsed_microseconds
              << " us\n"
              << "  Duration: " << options.duration_seconds
              << " seconds maximum\n"
              << "  Press Ctrl-C to stop early.\n";

    waitForDisplay(options.duration_seconds);
    const DrmCrtcRestoreResult restore_result = display.restore();
    destination.release();
    source.release();
    if (restore_result == DrmCrtcRestoreResult::kCrtcDisabled) {
        std::cout << "Cleanup: CRTC safely disabled; restart Weston\n";
    } else {
        std::cout << "Cleanup: no CRTC change was required\n";
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
                std::cout << "rga_drm_test " << CAMERA_DEMO_VERSION
                          << " (C++11, target=rk3568-aarch64-linux)\n";
                return EXIT_SUCCESS;
            }
        }
        runTest(parseOptions(argc, argv));
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "rga_drm_test: " << error.what() << '\n';
        std::cerr << "Try '" << argv[0] << " --help' for usage.\n";
        return EXIT_FAILURE;
    }
}
