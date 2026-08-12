#include "v4l2_buffer.hpp"

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <poll.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

/**
 * @brief 调用 ioctl，并在 EINTR 时自动重试。
 *
 * 当前 device 模块已有同类私有函数。后续实现 buffer queue 时，应将公共 Linux
 * syscall/error helper 抽取到内部公共模块，避免两个翻译单元长期重复。
 *
 * @param fd ioctl 操作的设备文件描述符。
 * @param request ioctl 请求码。
 * @param argument 指向请求参数结构体的指针。
 * @return 成功时返回非负值；失败时返回 -1 并保留 errno。
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
 * @brief 将系统调用失败时的 errno 转换为包含设备上下文的异常。
 *
 * 函数进入后立即保存 errno，避免后续字符串操作改变原始错误值。
 *
 * @param operation 失败的系统调用或 ioctl 名称。
 * @param device_path 发生错误的 V4L2 设备节点路径。
 * @return 可直接抛出的 std::runtime_error。
 */
std::runtime_error systemError(const std::string& operation,
                               const std::string& device_path)
{
    const int error = errno;
    return std::runtime_error(operation + " failed for " + device_path +
                              ": " + std::strerror(error) + " (errno=" +
                              std::to_string(error) + ")");
}

}  // namespace

V4L2BufferQueue::V4L2BufferQueue(const V4L2Device& device,
                                 const VideoFormat& format)
    : fd_(device.fd()),
      device_path_(device.path()),
      capture_type_(device.queryCapabilities().capture_type),
      plane_count_(format.plane_count)
{
    if (fd_ < 0) {
        throw std::invalid_argument("V4L2BufferQueue requires a valid device fd");
    }
    if (plane_count_ == 0U || plane_count_ > VIDEO_MAX_PLANES) {
        throw std::invalid_argument(
            "V4L2BufferQueue received an invalid memory plane count");
    }
}

V4L2BufferQueue::~V4L2BufferQueue()
{
    stopNoThrow();
    releaseMappings();
    releaseDriverBuffersNoThrow();
}

void V4L2BufferQueue::requestBuffers(std::uint32_t requested_count)
{
    if (requested_count == 0U) {
        throw std::invalid_argument("requested buffer count must be greater than zero");
    }
    if (state_ != V4L2BufferQueueState::Idle) {
        throw std::logic_error("requestBuffers requires the Idle state");
    }

    // VIDIOC_REQBUFS 同时选择该 fd 后续使用的 streaming memory model。这里使用
    // V4L2_MEMORY_MMAP，表示 buffer 由 V4L2 驱动分配，应用再通过 QUERYBUF 返回
    // 的 offset 将其映射到自己的虚拟地址空间。
    v4l2_requestbuffers request{};
    request.count = requested_count;
    request.type = capture_type_;
    request.memory = V4L2_MEMORY_MMAP;

    if (xioctl(fd_, VIDIOC_REQBUFS, &request) < 0) {
        throw systemError("VIDIOC_REQBUFS", device_path_);
    }

    // 驱动可以返回少于或多于 requested_count 的数量。count=0 表示没有分配到
    // buffer，后续不能继续 QUERYBUF 或 STREAMON。
    if (request.count == 0U) {
        throw std::runtime_error("VIDIOC_REQBUFS returned zero buffers for " +
                                 device_path_);
    }

    // 从这一刻起，内核 buffer 属于当前对象。后续 vector 分配、QUERYBUF 或 mmap
    // 任意一步抛出异常，都必须进入 catch 完成回滚。
    driver_buffers_allocated_ = true;

    try {
        buffers_.resize(static_cast<std::size_t>(request.count));

        for (std::uint32_t index = 0U; index < request.count; ++index) {
            BufferSlot& slot = buffers_[static_cast<std::size_t>(index)];
            slot.owned_by_application = true;

            if (capture_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
                // multi-planar API 要求应用提供 v4l2_plane 数组。buffer.length 在
                // ioctl 输入时表示数组容量，成功返回时表示驱动实际使用的 plane 数。
                v4l2_buffer buffer{};
                v4l2_plane planes[VIDEO_MAX_PLANES]{};
                buffer.type = capture_type_;
                buffer.memory = V4L2_MEMORY_MMAP;
                buffer.index = index;
                buffer.m.planes = planes;
                buffer.length = plane_count_;

                if (xioctl(fd_, VIDIOC_QUERYBUF, &buffer) < 0) {
                    throw systemError("VIDIOC_QUERYBUF", device_path_);
                }

                // QUERYBUF 返回的 memory plane 数应与此前 G_FMT 的 num_planes
                // 一致。不一致时继续映射会导致 plane 越界或错误的帧布局。
                if (buffer.length == 0U || buffer.length > plane_count_ ||
                    buffer.length > VIDEO_MAX_PLANES) {
                    throw std::runtime_error(
                        "VIDIOC_QUERYBUF returned an invalid plane count for " +
                        device_path_ + " at buffer " + std::to_string(index));
                }
                if (buffer.length != plane_count_) {
                    throw std::runtime_error(
                        "VIDIOC_QUERYBUF plane count differs from VIDIOC_G_FMT for " +
                        device_path_ + " at buffer " + std::to_string(index));
                }

                // 先确定 vector 大小，再执行 mmap。这样 mmap 成功后仅进行不会抛出
                // 的字段赋值，避免 vector 扩容失败导致刚映射的地址没有被记录。
                slot.planes.resize(static_cast<std::size_t>(buffer.length));

                for (std::uint32_t plane = 0U; plane < buffer.length; ++plane) {
                    if (planes[plane].length == 0U) {
                        throw std::runtime_error(
                            "VIDIOC_QUERYBUF returned a zero-length plane for " +
                            device_path_ + " at buffer " + std::to_string(index) +
                            ", plane " + std::to_string(plane));
                    }

                    // multi-planar MMAP 必须使用每个 v4l2_plane 自己的 mem_offset 和
                    // length，不能使用 single-planar 的 buffer.m.offset。
                    void* const address =
                        ::mmap(nullptr,
                               static_cast<std::size_t>(planes[plane].length),
                               PROT_READ | PROT_WRITE,
                               MAP_SHARED,
                               fd_,
                               static_cast<off_t>(planes[plane].m.mem_offset));
                    if (address == MAP_FAILED) {
                        throw systemError("mmap", device_path_);
                    }

                    MappedPlane& mapping =
                        slot.planes[static_cast<std::size_t>(plane)];
                    mapping.address = address;
                    mapping.length =
                        static_cast<std::size_t>(planes[plane].length);
                }
            } else {
                // single-planar API 的 offset 和 length 直接位于 v4l2_buffer 中。
                // 即使格式是 NV12，这里仍只有一个 memory plane，Y/UV image planes
                // 位于同一个映射区域内。
                v4l2_buffer buffer{};
                buffer.type = capture_type_;
                buffer.memory = V4L2_MEMORY_MMAP;
                buffer.index = index;

                if (xioctl(fd_, VIDIOC_QUERYBUF, &buffer) < 0) {
                    throw systemError("VIDIOC_QUERYBUF", device_path_);
                }
                if (buffer.length == 0U) {
                    throw std::runtime_error(
                        "VIDIOC_QUERYBUF returned a zero-length buffer for " +
                        device_path_ + " at buffer " + std::to_string(index));
                }

                // 在 mmap 之前创建唯一的 plane slot，理由与 multi-planar 分支相同：
                // mmap 成功后不能再执行可能抛出 bad_alloc 的容器扩容。
                slot.planes.resize(1U);
                void* const address =
                    ::mmap(nullptr,
                           static_cast<std::size_t>(buffer.length),
                           PROT_READ | PROT_WRITE,
                           MAP_SHARED,
                           fd_,
                           static_cast<off_t>(buffer.m.offset));
                if (address == MAP_FAILED) {
                    throw systemError("mmap", device_path_);
                }

                slot.planes[0U].address = address;
                slot.planes[0U].length =
                    static_cast<std::size_t>(buffer.length);
            }
        }

        // 只有全部 buffer 的全部 planes 都成功映射后，才允许进入下一生命周期状态。
        state_ = V4L2BufferQueueState::BuffersAllocated;
    } catch (...) {
        // mmap 必须先解除，再调用 REQBUFS(count=0)。部分驱动不支持 orphaned
        // buffers，如果仍有映射存在，反向顺序会使 count=0 返回 EBUSY。
        releaseMappings();
        releaseDriverBuffersNoThrow();
        throw;
    }
}

void V4L2BufferQueue::queueAll()
{
    if (state_ != V4L2BufferQueueState::BuffersAllocated) {
        throw std::logic_error("queueAll requires the BuffersAllocated state");
    }

    if (buffers_.empty()) {
        throw std::logic_error("queueAll requires at least one allocated buffer");
    }

    // queueAll 只允许从“全部 buffer 都由应用持有”的稳定状态开始，防止对已经排队
    // 的 index 重复执行 QBUF。
    for (std::size_t index = 0U; index < buffers_.size(); ++index) {
        if (!buffers_[index].owned_by_application) {
            throw std::logic_error(
                "queueAll found a buffer already owned by the V4L2 driver");
        }
    }

    try {
        for (std::size_t index = 0U; index < buffers_.size(); ++index) {
            queueBuffer(static_cast<std::uint32_t>(index));
        }
    } catch (...) {
        // 部分 QBUF 成功后无法单独撤销某个 index。释放整个 queue 可恢复为明确的
        // Idle 状态，调用方随后可以重新 requestBuffers()。
        releaseMappings();
        releaseDriverBuffersNoThrow();
        throw;
    }
}

void V4L2BufferQueue::start()
{
    if (state_ != V4L2BufferQueueState::BuffersAllocated) {
        throw std::logic_error("start requires the BuffersAllocated state");
    }

    if (buffers_.empty()) {
        throw std::logic_error("start requires at least one allocated buffer");
    }
    for (std::size_t index = 0U; index < buffers_.size(); ++index) {
        if (buffers_[index].owned_by_application) {
            throw std::logic_error("start requires every buffer to be queued");
        }
    }

    // STREAMON 的参数是 queue type 本身。该 type 必须与 S_FMT、REQBUFS、
    // QUERYBUF 和 QBUF 的 type 完全一致。
    v4l2_buf_type type = capture_type_;
    if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        throw systemError("VIDIOC_STREAMON", device_path_);
    }
    state_ = V4L2BufferQueueState::Streaming;
}

bool V4L2BufferQueue::waitForFrame(int timeout_ms) const
{
    if (state_ != V4L2BufferQueueState::Streaming) {
        throw std::logic_error("waitForFrame requires the Streaming state");
    }

    pollfd descriptor{};
    descriptor.fd = fd_;
    descriptor.events = POLLIN;

    const int result = ::poll(&descriptor, 1U, timeout_ms);
    if (result == 0) {
        return false;
    }
    if (result < 0) {
        // SIGINT/SIGTERM 会中断 poll。返回 false 让上层检查退出标志，避免无条件
        // 重试使程序无法响应 Ctrl+C。
        if (errno == EINTR) {
            return false;
        }
        throw systemError("poll", device_path_);
    }

    if ((descriptor.revents & POLLNVAL) != 0) {
        throw std::runtime_error("poll reported POLLNVAL for " + device_path_);
    }
    if ((descriptor.revents & POLLHUP) != 0) {
        throw std::runtime_error("poll reported POLLHUP for " + device_path_);
    }
    if ((descriptor.revents & POLLERR) != 0) {
        throw std::runtime_error("poll reported POLLERR for " + device_path_);
    }

    return (descriptor.revents & POLLIN) != 0;
}

bool V4L2BufferQueue::tryDequeue(CapturedFrame* frame)
{
    if (frame == nullptr) {
        throw std::invalid_argument("tryDequeue requires a non-null output frame");
    }
    if (state_ != V4L2BufferQueueState::Streaming) {
        throw std::logic_error("tryDequeue requires the Streaming state");
    }

    // 在 DQBUF 前完成 vector 分配。这样成功取回 buffer 后，metadata 复制不会再因
    // bad_alloc 抛出并遗失这个必须 requeue 的 buffer index。
    CapturedFrame result;
    result.planes.resize(static_cast<std::size_t>(plane_count_));

    v4l2_buffer buffer{};
    v4l2_plane planes[VIDEO_MAX_PLANES]{};
    buffer.type = capture_type_;
    buffer.memory = V4L2_MEMORY_MMAP;
    if (capture_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        buffer.m.planes = planes;
        buffer.length = plane_count_;
    }

    if (xioctl(fd_, VIDIOC_DQBUF, &buffer) < 0) {
        if (errno == EAGAIN) {
            return false;
        }
        throw systemError("VIDIOC_DQBUF", device_path_);
    }

    if (buffer.index >= buffers_.size()) {
        throw std::runtime_error("VIDIOC_DQBUF returned an invalid buffer index for " +
                                 device_path_);
    }

    BufferSlot& slot = buffers_[static_cast<std::size_t>(buffer.index)];
    if (slot.owned_by_application) {
        throw std::runtime_error(
            "VIDIOC_DQBUF returned a buffer already owned by the application");
    }

    result.buffer_index = buffer.index;
    result.sequence = buffer.sequence;
    result.timestamp = buffer.timestamp;
    result.flags = buffer.flags;

    if (capture_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        if (buffer.length != plane_count_ || buffer.length > VIDEO_MAX_PLANES ||
            slot.planes.size() != static_cast<std::size_t>(buffer.length)) {
            throw std::runtime_error(
                "VIDIOC_DQBUF returned an invalid multi-planar layout for " +
                device_path_);
        }

        for (std::uint32_t plane = 0U; plane < buffer.length; ++plane) {
            const MappedPlane& mapping =
                slot.planes[static_cast<std::size_t>(plane)];
            if (static_cast<std::size_t>(planes[plane].bytesused) > mapping.length ||
                planes[plane].data_offset > planes[plane].bytesused) {
                throw std::runtime_error(
                    "VIDIOC_DQBUF returned invalid plane byte counts for " +
                    device_path_);
            }

            CapturedPlane& captured =
                result.planes[static_cast<std::size_t>(plane)];
            captured.data = mapping.address;
            captured.mapped_length = mapping.length;
            captured.bytes_used = planes[plane].bytesused;
            captured.data_offset = planes[plane].data_offset;
        }
    } else {
        if (slot.planes.size() != 1U ||
            static_cast<std::size_t>(buffer.bytesused) > slot.planes[0U].length) {
            throw std::runtime_error(
                "VIDIOC_DQBUF returned invalid single-planar byte counts for " +
                device_path_);
        }

        result.planes.resize(1U);
        result.planes[0U].data = slot.planes[0U].address;
        result.planes[0U].mapped_length = slot.planes[0U].length;
        result.planes[0U].bytes_used = buffer.bytesused;
        result.planes[0U].data_offset = 0U;
    }

    // 只有 metadata 完整验证后才更新所有权并提交输出对象。
    slot.owned_by_application = true;
    *frame = std::move(result);
    return true;
}

void V4L2BufferQueue::requeue(std::uint32_t buffer_index)
{
    if (buffer_index >= buffers_.size()) {
        throw std::out_of_range("buffer index is outside the allocated pool");
    }
    if (state_ != V4L2BufferQueueState::Streaming) {
        throw std::logic_error("requeue requires the Streaming state");
    }
    if (!buffers_[buffer_index].owned_by_application) {
        throw std::logic_error("buffer is already owned by the V4L2 driver");
    }

    queueBuffer(buffer_index);
}

void V4L2BufferQueue::stop()
{
    if (state_ != V4L2BufferQueueState::Streaming) {
        throw std::logic_error("stop requires the Streaming state");
    }

    v4l2_buf_type type = capture_type_;
    if (xioctl(fd_, VIDIOC_STREAMOFF, &type) < 0) {
        throw systemError("VIDIOC_STREAMOFF", device_path_);
    }

    // STREAMOFF 清空队列并解除所有 buffer 的驱动锁定。mmap 保持有效，可在再次
    // queueAll() 后重新启动。
    for (std::size_t index = 0U; index < buffers_.size(); ++index) {
        buffers_[index].owned_by_application = true;
    }
    state_ = V4L2BufferQueueState::BuffersAllocated;
}

std::size_t V4L2BufferQueue::bufferCount() const noexcept
{
    return buffers_.size();
}

std::uint32_t V4L2BufferQueue::planeCount() const noexcept
{
    return plane_count_;
}

V4L2BufferQueueState V4L2BufferQueue::state() const noexcept
{
    return state_;
}

void V4L2BufferQueue::queueBuffer(std::uint32_t buffer_index)
{
    v4l2_buffer buffer{};
    v4l2_plane planes[VIDEO_MAX_PLANES]{};
    buffer.type = capture_type_;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = buffer_index;

    if (capture_type_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        // MMAP 模式下 fd/userptr 字段不由应用填写，但 multi-planar ioctl 仍要求提供
        // plane 数组和正确的数组容量。
        buffer.m.planes = planes;
        buffer.length = plane_count_;
    }

    if (xioctl(fd_, VIDIOC_QBUF, &buffer) < 0) {
        throw systemError("VIDIOC_QBUF", device_path_);
    }

    buffers_[static_cast<std::size_t>(buffer_index)].owned_by_application = false;
}

void V4L2BufferQueue::stopNoThrow() noexcept
{
    if (state_ != V4L2BufferQueueState::Streaming || fd_ < 0) {
        return;
    }

    // 析构路径不能抛异常。即使 STREAMOFF 失败，也继续 munmap；显式 stop() 才负责
    // 将错误报告给正常业务流程。
    v4l2_buf_type type = capture_type_;
    static_cast<void>(xioctl(fd_, VIDIOC_STREAMOFF, &type));
    for (std::size_t index = 0U; index < buffers_.size(); ++index) {
        buffers_[index].owned_by_application = true;
    }
    state_ = V4L2BufferQueueState::BuffersAllocated;
}

void V4L2BufferQueue::releaseMappings() noexcept
{
    for (std::size_t buffer = 0U; buffer < buffers_.size(); ++buffer) {
        std::vector<MappedPlane>& planes = buffers_[buffer].planes;
        for (std::size_t plane = 0U; plane < planes.size(); ++plane) {
            if (planes[plane].address != nullptr && planes[plane].length > 0U) {
                static_cast<void>(
                    ::munmap(planes[plane].address, planes[plane].length));
                planes[plane].address = nullptr;
                planes[plane].length = 0U;
            }
        }
    }
    buffers_.clear();
    state_ = V4L2BufferQueueState::Idle;
}

void V4L2BufferQueue::releaseDriverBuffersNoThrow() noexcept
{
    if (!driver_buffers_allocated_ || fd_ < 0) {
        return;
    }

    // 对 MMAP queue 调用 REQBUFS(count=0) 会释放或 orphan 驱动分配的全部 buffer。
    // reserved 字段必须保持清零，type/memory 必须与首次 REQBUFS 完全一致。
    v4l2_requestbuffers request{};
    request.count = 0U;
    request.type = capture_type_;
    request.memory = V4L2_MEMORY_MMAP;
    static_cast<void>(xioctl(fd_, VIDIOC_REQBUFS, &request));

    // 这是 noexcept 清理路径，无法把失败反馈给调用方。无论 ioctl 结果如何都清除
    // 本地所有权标记，避免析构过程中重复请求释放同一组 buffer。
    driver_buffers_allocated_ = false;
}
