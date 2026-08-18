#pragma once

#include "v4l2_buffer.hpp"
#include "v4l2_device.hpp"

#include <cstdint>
#include <memory>
#include <string>

/**
 * @brief 描述一个可重建 V4L2 capture session 的固定请求参数。
 *
 * 实际格式仍以每次 VIDIOC_G_FMT 返回的 VideoFormat 为准。该结构只保存跨 session
 * rebuild 不变的请求，不拥有任何 fd 或 buffer。
 */
struct CaptureSessionConfig {
    /** V4L2 capture node，例如 `/dev/video0`。 */
    std::string device_path;

    /** 请求的图像宽度，单位为像素。 */
    std::uint32_t width{0U};

    /** 请求的图像高度，单位为像素。 */
    std::uint32_t height{0U};

    /** 请求的 V4L2_PIX_FMT_* fourcc。 */
    std::uint32_t pixel_format{0U};

    /** 希望驱动分配的 MMAP buffer 数量，必须大于 0。 */
    std::uint32_t buffer_count{0U};

    /** 通过 VIDIOC_S_FMT 请求的颜色元数据。 */
    VideoColorMetadata requested_color;
};

/**
 * @brief 唯一拥有一个完整且已经 STREAMON 的 V4L2 capture session。
 *
 * session 包含 video fd、实际格式、MMAP buffer、导出的 DMA-BUF fd 和 streaming
 * queue。类不可复制或移动，也不是线程安全的。正常状态下 `queue()` 返回 Streaming
 * queue；`rebuild()` 会先销毁旧 queue，再关闭旧 fd，随后从设备节点重新创建全部
 * 资源。成员声明顺序保证 queue 始终先于它借用的 V4L2Device 析构。
 */
class CaptureSession {
public:
    /**
     * @brief 创建并启动第一代 capture session。
     * @param config 跨 rebuild 保持不变的设备、格式和 buffer 请求。
     * @throws std::invalid_argument 请求路径、尺寸、格式或 buffer 数无效时抛出。
     * @throws std::runtime_error open、格式协商、buffer ioctl、mmap、EXPBUF 或
     * STREAMON 失败时抛出。
     */
    explicit CaptureSession(const CaptureSessionConfig& config);

    /** @brief 按 queue 后于 device 的安全顺序自动释放全部 session 资源。 */
    ~CaptureSession();

    CaptureSession(const CaptureSession&) = delete;
    CaptureSession& operator=(const CaptureSession&) = delete;
    CaptureSession(CaptureSession&&) = delete;
    CaptureSession& operator=(CaptureSession&&) = delete;

    /**
     * @brief 销毁当前 V4L2 session，并使用原配置重新创建、导出 buffer 和 STREAMON。
     *
     * 旧 queue 可能处于错误后的不确定状态，因此使用其析构函数执行 best-effort
     * STREAMOFF/REQBUFS 回滚，而不是假定显式 stop() 一定可用。旧 fd 完全关闭后才
     * 打开替代 session，避免同一 video node 的两个 fd 争用硬件。
     *
     * @throws std::runtime_error 新 session 任一步创建失败时抛出；对象随后没有活动
     * queue，调用方应退出或经过外部退避后再次 rebuild。
     */
    void rebuild();

    /**
     * @brief 返回当前 Streaming buffer queue。
     * @return 由当前对象拥有的 queue 引用，下一次 rebuild 或析构时失效。
     * @throws std::logic_error 当前没有成功建立 session 时抛出。
     */
    V4L2BufferQueue& queue();

    /**
     * @brief 返回当前 session 由 VIDIOC_G_FMT 确认的实际格式。
     * @return 格式引用，下一次 rebuild 或析构时失效。
     * @throws std::logic_error 当前没有成功建立 session 时抛出。
     */
    const VideoFormat& format() const;

    /**
     * @brief 返回成功创建的 session 代数。
     * @return 第一代为 1；每次 rebuild 成功后递增。
     */
    std::uint64_t generation() const noexcept;

private:
    /** 所有 rebuild 使用的不可变请求副本。 */
    CaptureSessionConfig config_;

    /** 当前 V4L2 fd 的唯一所有者；必须声明在 queue_ 之前。 */
    std::unique_ptr<V4L2Device> device_;

    /** 当前 MMAP/DMA-BUF queue 的唯一所有者，借用 device_ 的 fd。 */
    std::unique_ptr<V4L2BufferQueue> queue_;

    /** 当前 session 的实际格式。 */
    VideoFormat format_;

    /** 已成功建立的 session 总代数。 */
    std::uint64_t generation_{0U};
};
