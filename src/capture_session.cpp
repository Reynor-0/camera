#include "capture_session.hpp"

#include <stdexcept>
#include <utility>

CaptureSession::CaptureSession(const CaptureSessionConfig& config)
    : config_(config)
{
    if (config_.device_path.empty() || config_.width == 0U ||
        config_.height == 0U || config_.pixel_format == 0U ||
        config_.buffer_count == 0U) {
        throw std::invalid_argument(
            "CaptureSession requires a device, format and buffer count");
    }
    rebuild();
}

CaptureSession::~CaptureSession() = default;

void CaptureSession::rebuild()
{
    // queue 借用 device fd，必须先销毁。错误后的 queue 可能仍是 Streaming，
    // V4L2BufferQueue 析构会执行不抛异常的 STREAMOFF 和 buffer 回收。
    queue_.reset();
    device_.reset();
    format_ = VideoFormat();

    // replacement_queue 声明在 replacement_device 之后，异常回滚时按反序先释放
    // queue，再关闭它借用的 fd。只有所有初始化步骤成功才提交为当前 session。
    std::unique_ptr<V4L2Device> replacement_device(
        new V4L2Device(config_.device_path));
    const VideoFormat replacement_format = replacement_device->setFormat(
        config_.width,
        config_.height,
        config_.pixel_format,
        config_.requested_color);
    std::unique_ptr<V4L2BufferQueue> replacement_queue(
        new V4L2BufferQueue(*replacement_device, replacement_format));
    replacement_queue->requestBuffers(config_.buffer_count);
    replacement_queue->exportDmaBuffers();
    replacement_queue->queueAll();
    replacement_queue->start();

    device_ = std::move(replacement_device);
    queue_ = std::move(replacement_queue);
    format_ = replacement_format;
    ++generation_;
}

V4L2BufferQueue& CaptureSession::queue()
{
    if (!queue_) {
        throw std::logic_error("CaptureSession has no active buffer queue");
    }
    return *queue_;
}

const VideoFormat& CaptureSession::format() const
{
    if (!queue_ || !device_) {
        throw std::logic_error("CaptureSession has no active format");
    }
    return format_;
}

std::uint64_t CaptureSession::generation() const noexcept
{
    return generation_;
}
