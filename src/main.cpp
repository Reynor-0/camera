#include "v4l2_device.hpp"

#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

#if defined(CAMERA_DEMO_TARGET_RK3568_AARCH64)
// RK3568 支持 64 位 ARM 用户态。本项目的默认交叉编译配置明确面向 AArch64。
// 如果误用了 32 位编译器，编译在这里直接失败，而不是部署到开发板后才发现
// ELF ABI 不匹配。若目标 rootfs 确实是 32 位，需要单独建立 armhf toolchain。
static_assert(sizeof(void*) == 8U,
              "RK3568 AArch64 build requires a 64-bit target compiler");
#endif

namespace {

/**
 * @brief 将 V4L2 的 32-bit fourcc 转换为四字符字符串。
 *
 * V4L2 fourcc 通过 v4l2_fourcc(a, b, c, d) 将四个字节编码到 uint32_t 中，
 * 例如 NV12 对应字符 `N`、`V`、`1`、`2`。按字节提取比直接输出整数更易阅读。
 *
 * @param value V4L2_PIX_FMT_* 或 v4l2_fmtdesc.pixelformat 的数值。
 * @return 长度固定为 4 的可读字符串。
 */
std::string fourccToString(std::uint32_t value)
{
    std::string result(4, ' ');
    result[0] = static_cast<char>(value & 0xffU);
    result[1] = static_cast<char>((value >> 8U) & 0xffU);
    result[2] = static_cast<char>((value >> 16U) & 0xffU);
    result[3] = static_cast<char>((value >> 24U) & 0xffU);
    return result;
}

/**
 * @brief 将命令行输入的四字符格式编码为 V4L2 fourcc。
 *
 * @param text 用户输入的格式字符串，例如 `NV12` 或 `YUYV`。
 * @return 可传给 VIDIOC_S_FMT 的 V4L2 fourcc 数值。
 * @throws std::invalid_argument 输入长度不是 4 时抛出。
 */
std::uint32_t parseFourcc(const std::string& text)
{
    if (text.size() != 4U) {
        throw std::invalid_argument("FOURCC must contain exactly four characters");
    }
    return v4l2_fourcc(text[0], text[1], text[2], text[3]);
}

/**
 * @brief 将一种离散尺寸或尺寸范围打印到标准输出。
 *
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
 * @brief 打印 camera_demo 支持的命令行形式和示例。
 *
 * @param program argv[0] 中的程序名称。
 */
void printUsage(const char* program)
{
    std::cerr << "Usage:\n"
              << "  " << program << " [video-device]\n"
              << "  " << program
              << " [video-device] <width> <height> <FOURCC>\n\n"
              << "Examples:\n"
              << "  " << program << " /dev/video0\n"
              << "  " << program << " /dev/video0 1920 1080 NV12\n";
}

}  // namespace

int main(int argc, char* argv[])
{
    // 支持三种形式：全部采用默认值、只指定设备、指定设备及待设置的完整格式。
    if (argc != 1 && argc != 2 && argc != 5) {
        printUsage(argv[0]);
        return EXIT_FAILURE;
    }

    const std::string device = argc >= 2 ? argv[1] : "/dev/video0";

    try {
        // V4L2Device 使用 RAII：从此处开始即使抛出异常，析构函数也会关闭 fd。
        V4L2Device camera(device);
        const DeviceCapabilities capabilities = camera.queryCapabilities();

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
                  << capabilities.effective_caps << std::dec << "\n\n";

        // 探测模式不会修改设备格式，只读取驱动公开的格式和尺寸列表。
        const std::vector<PixelFormatInfo> formats =
            camera.enumerateFormats();
        std::cout << "Formats (" << formats.size() << "):\n";
        for (const PixelFormatInfo& format : formats) {
            std::cout << "  " << fourccToString(format.pixel_format) << " ("
                      << format.description << ')';
            // COMPRESSED 表示 buffer 保存的是压缩码流，而不是可直接扫描输出的
            // 原始像素；EMULATED 表示格式由驱动软件转换提供，并非硬件原生格式。
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
            // std::stoul 属于 ISO C++11。转换或越界失败会抛出标准异常并由外层统一
            // 打印，不允许无效的宽高进入 ioctl。
            const std::uint32_t width =
                static_cast<std::uint32_t>(std::stoul(argv[2]));
            const std::uint32_t height =
                static_cast<std::uint32_t>(std::stoul(argv[3]));
            const std::uint32_t pixel_format = parseFourcc(argv[4]);
            // setFormat 返回驱动实际接受值；后续 buffer 分配必须使用 actual。
            const VideoFormat actual =
                camera.setFormat(width, height, pixel_format);

            std::cout << "\nRequested format: " << width << 'x' << height
                      << ' ' << fourccToString(pixel_format) << '\n'
                      << "Actual format:    " << actual.width << 'x'
                      << actual.height << ' '
                      << fourccToString(actual.pixel_format) << '\n'
                      << "Memory planes:    " << actual.plane_count << '\n';
            for (std::uint32_t plane = 0; plane < actual.plane_count; ++plane) {
                std::cout << "  Plane " << plane
                          << ": stride=" << actual.bytes_per_line[plane]
                          << ", size=" << actual.size_image[plane] << '\n';
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "camera_demo: " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
