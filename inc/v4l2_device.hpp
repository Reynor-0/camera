#pragma once

#include <linux/videodev2.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief 保存 V4L2 驱动最终确认的视频格式和内存布局。
 *
 * 调用 VIDIOC_S_FMT 之后，驱动可能修改应用请求的宽度、高度、像素格式和 stride。
 * 因此后续申请 buffer、创建 DRM framebuffer 时，必须使用本结构保存的实际值，
 * 不能继续使用应用最初请求的参数。
 */
struct VideoFormat {
    /** 驱动最终确认的有效图像宽度，单位为像素。 */
    std::uint32_t width{0};

    /** 驱动最终确认的有效图像高度，单位为像素。 */
    std::uint32_t height{0};

    /** V4L2 fourcc 像素格式，例如 V4L2_PIX_FMT_NV12。 */
    std::uint32_t pixel_format{0};

    /** V4L2 memory plane 数量，而不是颜色分量数量。 */
    std::uint32_t plane_count{0};

    /** 每个 memory plane 一行占用的字节数，其中可能包含硬件对齐填充。 */
    std::array<std::uint32_t, VIDEO_MAX_PLANES> bytes_per_line{};

    /** 驱动要求为每个 memory plane 分配的最小字节数。 */
    std::array<std::uint32_t, VIDEO_MAX_PLANES> size_image{};
};

/**
 * @brief 描述某种像素格式支持的图像尺寸或尺寸范围。
 *
 * VIDIOC_ENUM_FRAMESIZES 可能返回离散尺寸，也可能返回连续或按步长变化的范围。
 * 本结构统一保存这三种返回形式。
 */
struct FrameSizeInfo {
    /** @brief V4L2 帧尺寸枚举结果的类型。 */
    enum class Type {
        /** 驱动只支持一个明确的宽高组合。 */
        Discrete,

        /** 驱动在最小值和最大值之间按指定步长支持多种宽高。 */
        Stepwise,

        /** 驱动支持最小值和最大值之间的连续宽高。 */
        Continuous,
    };

    /** 当前尺寸描述的类型。 */
    Type type{Type::Discrete};

    /** 最小宽度；Discrete 模式下也代表唯一宽度。 */
    std::uint32_t min_width{0};

    /** 最大宽度；Discrete 模式下等于 min_width。 */
    std::uint32_t max_width{0};

    /** 宽度递增步长；仅 Stepwise 模式有效。 */
    std::uint32_t step_width{0};

    /** 最小高度；Discrete 模式下也代表唯一高度。 */
    std::uint32_t min_height{0};

    /** 最大高度；Discrete 模式下等于 min_height。 */
    std::uint32_t max_height{0};

    /** 高度递增步长；仅 Stepwise 模式有效。 */
    std::uint32_t step_height{0};
};

/**
 * @brief 描述 V4L2 capture queue 支持的一种像素格式。
 */
struct PixelFormatInfo {
    /** V4L2 fourcc 像素格式。 */
    std::uint32_t pixel_format{0};

    /** 驱动提供的可读格式名称。 */
    std::string description;

    /** v4l2_fmtdesc.flags，例如 COMPRESSED 或 EMULATED。 */
    std::uint32_t flags{0};

    /** 该像素格式支持的离散尺寸或尺寸范围。 */
    std::vector<FrameSizeInfo> frame_sizes;
};

/**
 * @brief 保存 VIDIOC_QUERYCAP 返回的设备身份和有效能力。
 */
struct DeviceCapabilities {
    /** 内核 V4L2 驱动名称。 */
    std::string driver;

    /** 设备的人类可读名称。 */
    std::string card;

    /** 设备在系统总线上的位置或唯一标识。 */
    std::string bus_info;

    /** v4l2_capability.capabilities 原始值。 */
    std::uint32_t capabilities{0};

    /** v4l2_capability.device_caps 原始值。 */
    std::uint32_t device_caps{0};

    /**
     * 应用实际应该检查的能力集合。
     *
     * 当 capabilities 含 V4L2_CAP_DEVICE_CAPS 时取 device_caps，否则取
     * capabilities。
     */
    std::uint32_t effective_caps{0};

    /** 当前设备使用的 single-planar 或 multi-planar capture queue 类型。 */
    v4l2_buf_type capture_type{V4L2_BUF_TYPE_VIDEO_CAPTURE};
};

/**
 * @brief 管理一个 V4L2 capture 设备并提供第一阶段所需的探测接口。
 *
 * 对象构造时打开设备，析构时自动关闭 fd。对象禁止复制，避免同一个 fd 被两个
 * 对象重复关闭。当前阶段只负责能力查询、格式枚举和格式设置；buffer queue 将由
 * 后续 V4L2 buffer 模块负责。
 */
class V4L2Device {
public:
    /**
     * @brief 打开一个 V4L2 设备节点。
     *
     * 设备以 O_RDWR、O_NONBLOCK 和 O_CLOEXEC 打开。O_NONBLOCK 使后续 DQBUF
     * 可以与 poll 配合；O_CLOEXEC 防止子进程意外继承设备 fd。
     *
     * @param path V4L2 设备节点路径，例如 `/dev/video0`。
     * @throws std::runtime_error 打开设备失败时抛出，错误信息包含 errno。
     */
    explicit V4L2Device(const std::string& path);

    /** @brief 关闭构造函数打开的设备 fd。 */
    ~V4L2Device();

    V4L2Device(const V4L2Device&) = delete;
    V4L2Device& operator=(const V4L2Device&) = delete;

    /**
     * @brief 查询并验证设备的 capture 和 streaming 能力。
     *
     * 该函数通过 VIDIOC_QUERYCAP 读取设备信息，并识别以下能力宏：
     *
     * - V4L2_CAP_VIDEO_CAPTURE：使用 `v4l2_pix_format` 的单 memory-plane
     *   capture API。即使 NV12 含 Y/UV 两个图像 plane，也可能存放在同一块内存中。
     * - V4L2_CAP_VIDEO_CAPTURE_MPLANE：使用 `v4l2_pix_format_mplane` 和
     *   `v4l2_plane[]` 的多 memory-plane capture API；每个 plane 可以有独立内存。
     * - V4L2_CAP_STREAMING：设备支持 REQBUFS、QBUF、DQBUF、STREAMON 等流式
     *   I/O，是后续 MMAP/DMA-BUF 采集的必要条件。
     * - V4L2_CAP_DEVICE_CAPS：`capabilities` 描述物理设备整体能力，而当前 video
     *   node 的能力要从 `device_caps` 字段读取。
     *
     * @return 设备身份、原始能力、有效能力和选中的 capture queue 类型。
     * @throws std::runtime_error ioctl 失败、设备不是 capture node，或不支持
     * streaming I/O 时抛出。
     */
    DeviceCapabilities queryCapabilities() const;

    /**
     * @brief 枚举 capture queue 支持的像素格式和帧尺寸。
     *
     * 函数先根据设备能力选择 VIDEO_CAPTURE 或 VIDEO_CAPTURE_MPLANE queue，随后
     * 反复调用 VIDIOC_ENUM_FMT，直到驱动以 EINVAL 表示枚举结束。每种格式再通过
     * VIDIOC_ENUM_FRAMESIZES 枚举支持的尺寸。
     *
     * @return 像素格式列表；驱动没有报告任何格式时返回空 vector。
     * @throws std::runtime_error 设备能力查询或格式枚举发生真实错误时抛出。
     */
    std::vector<PixelFormatInfo> enumerateFormats() const;

    /**
     * @brief 请求视频格式并返回驱动最终接受的格式。
     *
     * 函数先调用 VIDIOC_S_FMT 提交请求，再调用 VIDIOC_G_FMT 获取实际结果。
     * V4L2 允许驱动调整宽高、格式、stride 和 sizeimage，所以调用方必须使用返回值
     * 初始化后续 buffer 和 DRM framebuffer。
     *
     * @param width 请求的图像宽度，单位为像素。
     * @param height 请求的图像高度，单位为像素。
     * @param pixel_format 请求的 V4L2 fourcc，例如 V4L2_PIX_FMT_NV12。
     * @return 驱动最终接受的宽高、fourcc 和每个 memory plane 的布局。
     * @throws std::runtime_error 设备能力查询、VIDIOC_S_FMT 或 VIDIOC_G_FMT
     * 失败时抛出。
     */
    VideoFormat setFormat(std::uint32_t width,
                          std::uint32_t height,
                          std::uint32_t pixel_format);

    /**
     * @brief 获取已经打开的 V4L2 设备 fd。
     * @return 非负设备 fd；其所有权仍属于 V4L2Device，调用方不得关闭。
     */
    int fd() const noexcept;

    /**
     * @brief 获取构造时传入的设备节点路径。
     * @return 设备路径的常量引用，其生命周期不超过当前 V4L2Device 对象。
     */
    const std::string& path() const noexcept;

private:
    /**
     * @brief 查询并选择该设备应该使用的 capture queue 类型。
     * @return VIDEO_CAPTURE 或 VIDEO_CAPTURE_MPLANE。
     * @throws std::runtime_error 设备能力无效时抛出。
     */
    v4l2_buf_type captureType() const;

    /** 构造时传入的设备节点路径，用于诊断信息。 */
    std::string path_;

    /** 当前对象独占的 V4L2 设备文件描述符。 */
    int fd_{-1};
};
