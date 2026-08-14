#include "drm_device.hpp"

#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
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

/**
 * @brief 打印 drm_probe 命令行用法。
 * @param program argv[0] 中的程序名称。
 * @param output 接收帮助文本的输出流。
 */
void printUsage(const char* program, std::ostream& output)
{
    output << "drm_probe - read-only DRM/KMS resource probe\n\n"
           << "Usage:\n"
           << "  " << program << " [drm-device]\n"
           << "  " << program << " -h | --help\n"
           << "  " << program << " --version\n\n"
           << "Arguments:\n"
           << "  drm-device  DRM primary node (default /dev/dri/card0)\n\n"
           << "This command does not request DRM master, create framebuffers,\n"
           << "or change the current display state.\n";
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

        if (argc > 2) {
            printUsage(argv[0], std::cerr);
            return EXIT_FAILURE;
        }

        const std::string device_path =
            argc == 2 ? argv[1] : "/dev/dri/card0";
        const DrmDevice device(device_path);
        printProbeResult(device.path(), device.probe());
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "drm_probe: " << error.what() << '\n';
        std::cerr << "Try '" << argv[0] << " --help' for usage.\n";
        return EXIT_FAILURE;
    }
}
