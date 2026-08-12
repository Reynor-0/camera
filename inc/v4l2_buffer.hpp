#pragma once

#include "v4l2_device.hpp"

#include <linux/videodev2.h>
#include <sys/time.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief 表示 V4L2 MMAP capture queue 当前所处的生命周期状态。
 *
 * 状态转换必须遵循：Idle -> BuffersAllocated -> Streaming。停止采集后回到
 * BuffersAllocated；释放所有 mmap 和内核 buffer 后回到 Idle。
 */
enum class V4L2BufferQueueState {
    /** 尚未通过 VIDIOC_REQBUFS 向驱动申请 buffer。 */
    Idle,

    /** buffer 已申请并 mmap，但尚未开始或已经停止 streaming。 */
    BuffersAllocated,

    /** 已成功调用 VIDIOC_STREAMON，驱动可能正在写入已排队的 buffer。 */
    Streaming,
};

/**
 * @brief 描述一次 DQBUF 返回的单个 memory plane。
 *
 * `data` 指向 mmap 得到的虚拟地址。该指针不拥有内存，其有效期受
 * V4L2BufferQueue 和对应 buffer 的所有权状态约束：只有 buffer 已 DQBUF 且尚未
 * requeue 时，应用才能读取这段内存。
 */
struct CapturedPlane {
    /** 当前 plane 的 mmap 起始地址；调用方不得 free 或 munmap。 */
    const void* data{nullptr};

    /** 当前 mmap 区域的总长度，单位为字节。 */
    std::size_t mapped_length{0U};

    /** 驱动为本帧写入的有效字节数，对应 v4l2_plane.bytesused。 */
    std::uint32_t bytes_used{0U};

    /** 有效图像数据相对 mmap 起始地址的偏移，单位为字节。 */
    std::uint32_t data_offset{0U};
};

/**
 * @brief 保存一次 VIDIOC_DQBUF 返回的帧元数据和可读 plane 视图。
 *
 * CapturedFrame 不拥有 V4L2 buffer。调用方处理完帧后，必须将 `buffer_index`
 * 传给 V4L2BufferQueue::requeue()。requeue 成功后，本对象中的所有 `data` 指针立即
 * 失效，因为 buffer 的所有权已经重新交给驱动。
 */
struct CapturedFrame {
    /** 驱动 buffer pool 中的 buffer 下标。 */
    std::uint32_t buffer_index{0U};

    /** 驱动生成的帧序号，可用于发现丢帧或流水线重启。 */
    std::uint32_t sequence{0U};

    /** 驱动记录的采集时间戳；时钟类型由 v4l2_buffer.flags 描述。 */
    timeval timestamp{};

    /** v4l2_buffer.flags 原始值，例如时间戳类型或 ERROR 标志。 */
    std::uint32_t flags{0U};

    /** 本帧包含的一个或多个 memory plane。 */
    std::vector<CapturedPlane> planes;
};

/**
 * @brief 管理 V4L2 MMAP capture buffer 的申请、排队、采集和释放。
 *
 * 本类借用 V4L2Device 的 fd，不拥有也不关闭该 fd。因此 V4L2Device 对象必须比
 * V4L2BufferQueue 活得更久。本类拥有通过 VIDIOC_REQBUFS 获得的内核 buffer 以及
 * QUERYBUF 后建立的 mmap 映射，并在析构时执行 best-effort STREAMOFF 和 munmap。
 *
 * 本类不可复制，当前实现也不提供移动语义。它不是线程安全的；所有方法应由同一
 * 个事件循环线程调用。
 *
 * @note 当前实现覆盖 MMAP buffer 的完整采集生命周期。DMA-BUF 导出仍属于后续
 * 阶段，需按照 docs/v4l2_mmap_capture_plan.md 完成本机和目标板验收后再加入。
 */
class V4L2BufferQueue {
public:
    /**
     * @brief 创建一个尚未申请 buffer 的 MMAP capture queue 管理器。
     *
     * 构造函数会保存设备 fd、路径、capture type 和实际 plane 数，但不会立即调用
     * VIDIOC_REQBUFS。调用方必须先通过 V4L2Device::setFormat() 取得 `format`。
     *
     * @param device 已打开并支持 streaming 的 V4L2 capture 设备。该对象必须比
     * 当前 queue 活得更久。
     * @param format VIDIOC_G_FMT 返回的实际视频格式和 memory plane 数量。
     * @throws std::invalid_argument fd 无效、plane 数为 0 或超过 VIDEO_MAX_PLANES
     * 时抛出。
     */
    V4L2BufferQueue(const V4L2Device& device, const VideoFormat& format);

    /**
     * @brief 停止可能仍在运行的 stream，并释放所有 mmap 映射。
     *
     * 析构函数不得抛出异常。发生清理错误时只能执行 best-effort 释放；正常业务
     * 路径应在析构前显式调用 stop()，以便取得错误信息。
     */
    ~V4L2BufferQueue();

    V4L2BufferQueue(const V4L2BufferQueue&) = delete;
    V4L2BufferQueue& operator=(const V4L2BufferQueue&) = delete;

    /**
     * @brief 申请 V4L2 MMAP buffers，并将每个 memory plane 映射到进程地址空间。
     *
     * 实现需要依次调用 VIDIOC_REQBUFS、VIDIOC_QUERYBUF 和 mmap。驱动可以返回少于
     * 或多于 requested_count 的数量，必须使用 `v4l2_requestbuffers.count` 的实际
     * 返回值。multi-planar API 必须为 QUERYBUF 提供 `v4l2_plane[]`。
     *
     * @param requested_count 希望申请的 buffer 数量；第一版建议为 4，且必须大于
     * 0。
     * @throws std::invalid_argument requested_count 为 0 时抛出。
     * @throws std::logic_error queue 状态不是 Idle 时抛出。
     * @throws std::runtime_error ioctl/mmap 失败或驱动未返回 buffer 时抛出。
     */
    void requestBuffers(std::uint32_t requested_count);

    /**
     * @brief 将当前 pool 中所有空 buffer 排入 capture queue。
     *
     * VIDIOC_QBUF 成功后，对应 buffer 的所有权转移给 V4L2 驱动；应用不得继续
     * 读取或写入其 mmap 地址，直到 VIDIOC_DQBUF 将 buffer 归还。
     *
     * @throws std::logic_error 尚未申请 buffer、正在 streaming 或存在已排队 buffer
     * 时抛出。
     * @throws std::runtime_error 任意 VIDIOC_QBUF 失败时抛出。
     */
    void queueAll();

    /**
     * @brief 对当前 capture queue 执行 VIDIOC_STREAMON。
     *
     * 调用前必须已成功 queueAll()。传给 ioctl 的 `v4l2_buf_type` 必须与 S_FMT、
     * REQBUFS 和 QBUF 使用的 type 完全一致。
     *
     * @throws std::logic_error 状态不正确或仍有 buffer 未排队时抛出。
     * @throws std::runtime_error VIDIOC_STREAMON 失败时抛出。
     */
    void start();

    /**
     * @brief 使用 poll 等待 capture fd 可读。
     *
     * poll 成功只说明驱动可能允许 DQBUF；调用方随后仍需处理 VIDIOC_DQBUF 的
     * EAGAIN。POLLERR、POLLHUP 和 POLLNVAL 必须视为设备或 stream 异常。
     *
     * @param timeout_ms 等待超时，单位毫秒；0 表示立即返回，负数表示无限等待。
     * @return fd 可读时返回 true；正常超时或 poll 被信号中断时返回 false。调用方
     * 可在 false 后检查自己的退出标志。
     * @throws std::logic_error queue 不处于 Streaming 时抛出。
     * @throws std::runtime_error poll 发生真实错误时抛出。
     */
    bool waitForFrame(int timeout_ms) const;

    /**
     * @brief 尝试从 capture queue 取出一个已经由驱动写完的 buffer。
     *
     * DQBUF 成功后 buffer 所有权转移回应用，本函数返回的 plane 地址可以读取。
     * 对 multi-planar API，必须从每个 v4l2_plane 读取 bytesused 和 data_offset。
     *
     * @param frame 非空输出参数。成功时写入帧序号、时间戳、flags、buffer index
     * 和每个 plane 的只读视图；返回 false 时保持调用前内容不变。
     * @return 成功 DQBUF 时返回 true；非阻塞 ioctl 返回 EAGAIN 时返回 false。
     * @throws std::logic_error queue 不处于 Streaming 时抛出。
     * @throws std::invalid_argument frame 是空指针时抛出。
     * @throws std::runtime_error VIDIOC_DQBUF 遇到 EAGAIN 以外的错误，或驱动返回
     * 非法 buffer/plane metadata 时抛出。
     */
    bool tryDequeue(CapturedFrame* frame);

    /**
     * @brief 将应用已经处理完的 buffer 重新排入 capture queue。
     *
     * VIDIOC_QBUF 成功后，先前 CapturedFrame 中属于该 index 的 data 指针立即失效。
     * 后续接入 DRM 时，只有确认显示控制器不再扫描该 buffer 后才能调用本函数。
     *
     * @param buffer_index tryDequeue() 返回的 buffer 下标。
     * @throws std::out_of_range buffer_index 不属于当前 pool 时抛出。
     * @throws std::logic_error 状态不正确或 buffer 不属于应用时抛出。
     * @throws std::runtime_error VIDIOC_QBUF 失败时抛出。
     */
    void requeue(std::uint32_t buffer_index);

    /**
     * @brief 通过 VIDIOC_STREAMOFF 停止采集并取回所有已排队 buffer。
     *
     * STREAMOFF 会清空 incoming/outgoing queue，并解除驱动对 buffer 的锁定。成功
     * 后状态回到 BuffersAllocated；mmap 仍然保留，可在重新 queueAll() 后再次启动。
     *
     * @throws std::logic_error queue 不处于 Streaming 时抛出。
     * @throws std::runtime_error VIDIOC_STREAMOFF 失败时抛出。
     */
    void stop();

    /**
     * @brief 获取驱动实际分配的 buffer 数量。
     * @return Idle 状态下返回 0，否则返回 REQBUFS 的实际 count。
     */
    std::size_t bufferCount() const noexcept;

    /**
     * @brief 获取每个 buffer 的 memory plane 数量。
     * @return single-planar 通常为 1，multi-planar 使用 G_FMT 的 num_planes。
     */
    std::uint32_t planeCount() const noexcept;

    /**
     * @brief 获取当前 queue 生命周期状态。
     * @return Idle、BuffersAllocated 或 Streaming。
     */
    V4L2BufferQueueState state() const noexcept;

private:
    /** @brief 保存一个 mmap memory plane 的拥有型映射。 */
    struct MappedPlane {
        /** mmap 返回的地址；MAP_FAILED 不应存入成功映射列表。 */
        void* address{nullptr};

        /** mmap 区域长度，单位为字节。 */
        std::size_t length{0U};
    };

    /** @brief 保存一个 V4L2 buffer 的全部 memory plane 和应用侧所有权状态。 */
    struct BufferSlot {
        /** 当前 buffer 通过 QUERYBUF 发现的 memory planes。 */
        std::vector<MappedPlane> planes;

        /** true 表示 buffer 已 DQBUF 且当前由应用持有。 */
        bool owned_by_application{true};
    };

    /**
     * @brief 将一个当前由应用持有的 buffer 交给 V4L2 驱动。
     *
     * @param buffer_index 当前 pool 内的合法 buffer 下标。
     * @throws std::runtime_error VIDIOC_QBUF 失败时抛出。
     */
    void queueBuffer(std::uint32_t buffer_index);

    /**
     * @brief 在析构路径中尽力执行 STREAMOFF，不抛出异常。
     */
    void stopNoThrow() noexcept;

    /**
     * @brief munmap 所有成功建立的映射并清空本地 slot 列表。
     */
    void releaseMappings() noexcept;

    /**
     * @brief 使用 VIDIOC_REQBUFS(count=0) 尽力释放驱动分配的 MMAP buffers。
     *
     * 调用前必须先解除全部 mmap，否则不支持 orphaned buffers 的驱动可能返回
     * EBUSY。该函数用于析构和初始化失败回滚，因此不抛出异常。
     */
    void releaseDriverBuffersNoThrow() noexcept;

    /** 从 V4L2Device 借用的 fd；本类不得 close。 */
    int fd_{-1};

    /** 设备路径副本，用于构造包含上下文的错误信息。 */
    std::string device_path_;

    /** 所有 buffer ioctl 必须使用的 capture queue 类型。 */
    v4l2_buf_type capture_type_{V4L2_BUF_TYPE_VIDEO_CAPTURE};

    /** G_FMT 返回的实际 memory plane 数量。 */
    std::uint32_t plane_count_{0U};

    /** 当前 buffer queue 的生命周期状态。 */
    V4L2BufferQueueState state_{V4L2BufferQueueState::Idle};

    /** 驱动实际分配并由本对象管理的 buffer slots。 */
    std::vector<BufferSlot> buffers_;

    /** true 表示本对象已通过 REQBUFS 获得、且尚未以 count=0 释放内核 buffers。 */
    bool driver_buffers_allocated_{false};
};
