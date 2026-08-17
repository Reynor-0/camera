#include "v4l2_device.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

/**
 * @brief 调用 ioctl，并在系统调用被信号中断时自动重试。
 *
 * Linux 系统调用可能因为线程收到信号而返回 EINTR。EINTR 不代表设备失败，调用方
 * 通常应该重新发起同一个 ioctl。其他错误保持原 errno 并返回给调用方处理。
 *
 * @param fd ioctl 操作的设备文件描述符。
 * @param request ioctl 请求码，例如 VIDIOC_QUERYCAP。
 * @param argument 指向该请求对应参数结构体的指针。
 * @return 成功时返回 ioctl 的非负结果；失败时返回 -1，并保留 errno。
 */
int xioctl(int fd, unsigned long request, void* argument)
{
    int result;
    do {
        result = ::ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);
    return result;
}

/**
 * @brief 将当前 errno 转换成包含操作名和设备路径的异常。
 *
 * 必须在失败的系统调用之后立即保存 errno，避免字符串拼接等后续操作改变它。
 *
 * @param operation 失败的操作名称，例如 `VIDIOC_QUERYCAP`。
 * @param path 发生错误的设备节点路径。
 * @return 可直接抛出的 std::runtime_error 对象。
 */
std::runtime_error systemError(const std::string& operation,
                               const std::string& path)
{
    const int error = errno;
    return std::runtime_error(operation + " failed for " + path + ": " +
                              std::strerror(error) + " (errno=" +
                              std::to_string(error) + ")");
}

/**
 * @brief 将项目中的 32 位 V4L2 颜色枚举安全写入 multi-planar UAPI 字段。
 *
 * `v4l2_pix_format_mplane` 将 xfer_func、ycbcr_enc 和 quantization 定义为
 * `__u8`，而 single-planar 结构及本项目的公共元数据结构使用 32 位字段。这里
 * 先验证范围再显式收窄，避免无效枚举值被静默截断后交给驱动。
 *
 * @param value 待写入的 V4L2 枚举值。
 * @param field_name 用于异常信息的字段名。
 * @return 可安全写入 multi-planar 格式结构的 8 位值。
 * @throws std::invalid_argument value 超出内核 UAPI 字段可表达范围时抛出。
 */
std::uint8_t checkedMplaneColorField(std::uint32_t value,
                                     const char* field_name)
{
    const std::uint32_t maximum =
        static_cast<std::uint32_t>(std::numeric_limits<std::uint8_t>::max());
    if (value > maximum) {
        throw std::invalid_argument(
            std::string("V4L2 multi-planar color field ") + field_name +
            " is out of range: " + std::to_string(value));
    }
    return static_cast<std::uint8_t>(value);
}

/**
 * @brief 从 v4l2_capability 中取得当前 video node 的有效能力位。
 *
 * V4L2_CAP_DEVICE_CAPS 表示 `capabilities` 字段描述的是一个物理设备的整体能力，
 * 而当前打开的 `/dev/videoX` 节点能力存放在 `device_caps`。旧驱动没有该标志时，
 * 应直接使用 `capabilities`。
 *
 * @param capability VIDIOC_QUERYCAP 返回的内核结构体。
 * @return 应用于当前 video node 的 V4L2_CAP_* 位集合。
 */
std::uint32_t effectiveCapabilities(const v4l2_capability& capability)
{
    if ((capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0U) {
        return capability.device_caps;
    }
    return capability.capabilities;
}

/**
 * @brief 根据设备能力选择 single-planar 或 multi-planar capture API。
 *
 * 这里的 plane 指独立的 memory plane，而不是 Y、U、V 颜色分量。一个 single-
 * planar NV12 buffer 仍然包含 Y 和 UV 两个图像 plane，只是它们共享同一块内存。
 * 如果驱动同时声明两种 API，本项目优先使用 multi-planar，以保留完整的 per-plane
 * stride、size 和后续 DMA-BUF fd 信息。
 *
 * @param capabilities 当前 video node 的有效 V4L2_CAP_* 能力位。
 * @return 对应 ioctl 结构体中 `type` 字段的 capture queue 类型。
 * @throws std::runtime_error 设备不支持任何 capture API 时抛出。
 */
v4l2_buf_type selectCaptureType(std::uint32_t capabilities)
{
    // V4L2_CAP_VIDEO_CAPTURE_MPLANE 表示驱动使用多 memory-plane API：格式通过
    // v4l2_pix_format_mplane 表达，buffer 数据则通过 v4l2_plane 数组表达。
    if ((capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) != 0U) {
        return V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    }

    // V4L2_CAP_VIDEO_CAPTURE 表示驱动使用传统 single-planar API：格式通过
    // v4l2_pix_format 表达，一个 buffer 对应一块连续内存。
    if ((capabilities & V4L2_CAP_VIDEO_CAPTURE) != 0U) {
        return V4L2_BUF_TYPE_VIDEO_CAPTURE;
    }
    throw std::runtime_error(
        "device is neither VIDEO_CAPTURE nor VIDEO_CAPTURE_MPLANE");
}

/**
 * @brief 枚举一个 V4L2 像素格式支持的所有帧尺寸。
 *
 * VIDIOC_ENUM_FRAMESIZES 使用从 0 开始的 index。V4L2 约定当 index 超出范围时返回
 * EINVAL，因此该错误在枚举过程中表示正常结束，而不是运行失败。
 *
 * @param fd 已经打开的 V4L2 设备 fd。
 * @param pixel_format 要查询的 V4L2 fourcc。
 * @param path 设备路径，仅用于生成错误信息。
 * @return 该格式支持的离散尺寸或尺寸范围；驱动不支持此 ioctl 时可能为空。
 * @throws std::runtime_error 枚举过程中遇到 EINVAL 以外的错误时抛出。
 */
std::vector<FrameSizeInfo> enumerateFrameSizes(int fd,
                                                std::uint32_t pixel_format,
                                                const std::string& path)
{
    std::vector<FrameSizeInfo> result;

    for (std::uint32_t index = 0;; ++index) {
        v4l2_frmsizeenum size{};
        size.index = index;
        size.pixel_format = pixel_format;

        if (xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) < 0) {
            // 枚举类 ioctl 使用 EINVAL 表示当前 index 已越过最后一项。
            if (errno == EINVAL) {
                break;
            }
            throw systemError("VIDIOC_ENUM_FRAMESIZES", path);
        }

        FrameSizeInfo info;
        if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
            // 离散模式表示驱动明确支持一个 width x height 组合。
            info.type = FrameSizeInfo::Type::Discrete;
            info.min_width = size.discrete.width;
            info.max_width = size.discrete.width;
            info.min_height = size.discrete.height;
            info.max_height = size.discrete.height;
        } else {
            // Stepwise/Continuous 共用 v4l2_frmsize_stepwise union 成员。Continuous
            // 可理解为步长约束不需要由应用展示或强制。
            info.type = size.type == V4L2_FRMSIZE_TYPE_CONTINUOUS
                            ? FrameSizeInfo::Type::Continuous
                            : FrameSizeInfo::Type::Stepwise;
            info.min_width = size.stepwise.min_width;
            info.max_width = size.stepwise.max_width;
            info.step_width = size.stepwise.step_width;
            info.min_height = size.stepwise.min_height;
            info.max_height = size.stepwise.max_height;
            info.step_height = size.stepwise.step_height;
        }
        result.push_back(info);
    }

    return result;
}

}  // namespace

V4L2Device::V4L2Device(const std::string& path) : path_(path)
{
    // O_RDWR 是 streaming I/O 常见且兼容性最好的打开方式；O_NONBLOCK 让后续
    // DQBUF 在没有帧时返回 EAGAIN，而不是永久阻塞；O_CLOEXEC 防止 fd 泄漏到
    // exec 启动的其他程序。
    fd_ = ::open(path_.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
        throw systemError("open", path_);
    }
}

V4L2Device::~V4L2Device()
{
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

int V4L2Device::fd() const noexcept
{
    return fd_;
}

const std::string& V4L2Device::path() const noexcept
{
    return path_;
}

DeviceCapabilities V4L2Device::queryCapabilities() const
{
    // 所有传给 V4L2 ioctl 的结构体都必须清零，包括 reserved 字段。花括号初始化
    // 是 ISO C++11 支持的值初始化写法。
    v4l2_capability capability{};
    if (xioctl(fd_, VIDIOC_QUERYCAP, &capability) < 0) {
        throw systemError("VIDIOC_QUERYCAP", path_);
    }

    DeviceCapabilities result;
    result.driver = reinterpret_cast<const char*>(capability.driver);
    result.card = reinterpret_cast<const char*>(capability.card);
    result.bus_info = reinterpret_cast<const char*>(capability.bus_info);
    result.capabilities = capability.capabilities;
    result.device_caps = capability.device_caps;
    // 不应始终直接判断 cap.capabilities；新式驱动可能要求使用 device_caps。
    result.effective_caps = effectiveCapabilities(capability);
    result.capture_type = selectCaptureType(result.effective_caps);

    // V4L2_CAP_STREAMING 表示驱动实现了 REQBUFS/QBUF/DQBUF/STREAMON/STREAMOFF。
    // 本项目后续要使用 MMAP buffer 并导出 DMA-BUF，因此它是必要能力。
    if ((result.effective_caps & V4L2_CAP_STREAMING) == 0U) {
        throw std::runtime_error(path_ + " does not support streaming I/O");
    }

    return result;
}

v4l2_buf_type V4L2Device::captureType() const
{
    return queryCapabilities().capture_type;
}

std::vector<PixelFormatInfo> V4L2Device::enumerateFormats() const
{
    std::vector<PixelFormatInfo> result;
    const v4l2_buf_type type = captureType();

    for (std::uint32_t index = 0;; ++index) {
        // VIDIOC_ENUM_FMT 必须使用 v4l2_fmtdesc，而不是用于 S_FMT/G_FMT 的
        // v4l2_format。type 必须与后续 buffer queue 的 capture type 一致。
        v4l2_fmtdesc description{};
        description.index = index;
        description.type = type;

        if (xioctl(fd_, VIDIOC_ENUM_FMT, &description) < 0) {
            // 与其他 V4L2 枚举 ioctl 一样，EINVAL 在这里表示已经枚举完毕。
            if (errno == EINVAL) {
                break;
            }
            throw systemError("VIDIOC_ENUM_FMT", path_);
        }

        PixelFormatInfo info;
        info.pixel_format = description.pixelformat;
        info.description =
            reinterpret_cast<const char*>(description.description);
        info.flags = description.flags;
        info.frame_sizes =
            enumerateFrameSizes(fd_, description.pixelformat, path_);
        result.push_back(info);
    }

    return result;
}

VideoFormat V4L2Device::setFormat(std::uint32_t width,
                                  std::uint32_t height,
                                  std::uint32_t pixel_format,
                                  const VideoColorMetadata& requested_color)
{
    const v4l2_buf_type type = captureType();
    // v4l2_format 内部是 union。必须先设置 type，再根据 type 访问 pix 或 pix_mp，
    // 不能混用两套 API 的成员。
    v4l2_format format{};
    format.type = type;

    if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        // multi-planar API 使用 pix_mp。num_planes、bytesperline 和 sizeimage 由
        // 驱动在 S_FMT/G_FMT 后填写，应用此时无需自行猜测。
        format.fmt.pix_mp.width = width;
        format.fmt.pix_mp.height = height;
        format.fmt.pix_mp.pixelformat = pixel_format;
        format.fmt.pix_mp.field = V4L2_FIELD_ANY;
        format.fmt.pix_mp.colorspace = requested_color.colorspace;
        format.fmt.pix_mp.xfer_func =
            checkedMplaneColorField(requested_color.xfer_func, "xfer_func");
        format.fmt.pix_mp.ycbcr_enc =
            checkedMplaneColorField(requested_color.ycbcr_enc, "ycbcr_enc");
        format.fmt.pix_mp.quantization = checkedMplaneColorField(
            requested_color.quantization, "quantization");
    } else {
        // single-planar API 使用 pix，整帧图像对应一个 memory plane。
        format.fmt.pix.width = width;
        format.fmt.pix.height = height;
        format.fmt.pix.pixelformat = pixel_format;
        format.fmt.pix.field = V4L2_FIELD_ANY;
        format.fmt.pix.colorspace = requested_color.colorspace;
        format.fmt.pix.xfer_func = requested_color.xfer_func;
        format.fmt.pix.ycbcr_enc = requested_color.ycbcr_enc;
        format.fmt.pix.quantization = requested_color.quantization;
    }

    if (xioctl(fd_, VIDIOC_S_FMT, &format) < 0) {
        throw systemError("VIDIOC_S_FMT", path_);
    }

    // 明确重新读取一次。V4L2 允许 S_FMT 自动修正不支持或未对齐的请求，后续只能
    // 使用驱动最终确认的格式与内存布局。
    format = {};
    format.type = type;
    if (xioctl(fd_, VIDIOC_G_FMT, &format) < 0) {
        throw systemError("VIDIOC_G_FMT", path_);
    }

    VideoFormat result;
    if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        const v4l2_pix_format_mplane& actual = format.fmt.pix_mp;
        result.width = actual.width;
        result.height = actual.height;
        result.pixel_format = actual.pixelformat;
        result.field = actual.field;
        result.colorspace = actual.colorspace;
        result.xfer_func = actual.xfer_func;
        result.ycbcr_enc = actual.ycbcr_enc;
        result.quantization = actual.quantization;
        // 防御性限制数组下标。符合规范的驱动不会返回超过 VIDEO_MAX_PLANES 的值。
        result.plane_count =
            std::min<std::uint32_t>(actual.num_planes, VIDEO_MAX_PLANES);

        for (std::uint32_t plane = 0; plane < result.plane_count; ++plane) {
            result.bytes_per_line[plane] =
                actual.plane_fmt[plane].bytesperline;
            result.size_image[plane] = actual.plane_fmt[plane].sizeimage;
        }
    } else {
        const v4l2_pix_format& actual = format.fmt.pix;
        result.width = actual.width;
        result.height = actual.height;
        result.pixel_format = actual.pixelformat;
        result.field = actual.field;
        result.colorspace = actual.colorspace;
        result.xfer_func = actual.xfer_func;
        result.ycbcr_enc = actual.ycbcr_enc;
        result.quantization = actual.quantization;
        result.plane_count = 1;
        result.bytes_per_line[0] = actual.bytesperline;
        result.size_image[0] = actual.sizeimage;
    }

    return result;
}
