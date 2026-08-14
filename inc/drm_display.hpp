#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

/**
 * @brief 拥有一个线性 XRGB8888 DRM dumb buffer 及其 framebuffer ID。
 *
 * 构造过程依次执行 CREATE_DUMB、MAP_DUMB、mmap 和 drmModeAddFB2，但不会把
 * framebuffer 绑定到 plane 或 CRTC，也不会申请 DRM master。因此本类可用于
 * Weston 运行期间验证 buffer 生命周期，而不会改变屏幕内容。
 *
 * 本类借用 DrmDevice 的 fd，不负责 close；DrmDevice 必须比本对象活得更久。本类
 * 拥有 GEM handle、mmap 地址和 framebuffer ID，不可复制或移动，也不是线程安全的。
 */
class DrmDumbFramebuffer {
public:
    /**
     * @brief 创建并映射一个线性 XRGB8888 dumb framebuffer。
     *
     * 驱动可能为每行增加对齐填充，因此必须保存 CREATE_DUMB 返回的实际 pitch 和
     * size，不能假定 pitch 等于 width * 4。
     *
     * @param drm_fd 从 DrmDevice 借用的有效 DRM primary node fd。
     * @param width framebuffer 宽度，单位为像素，必须大于 0。
     * @param height framebuffer 高度，单位为像素，必须大于 0。
     * @throws std::invalid_argument fd 无效或宽高为 0 时抛出。
     * @throws std::runtime_error DRM ioctl、mmap 或 drmModeAddFB2 失败，或者驱动
     * 返回无法映射的大小时抛出；已创建的部分资源会自动回滚。
     */
    DrmDumbFramebuffer(int drm_fd,
                       std::uint32_t width,
                       std::uint32_t height);

    /** @brief 以 best-effort 顺序释放 framebuffer、mmap 和 GEM handle。 */
    ~DrmDumbFramebuffer();

    DrmDumbFramebuffer(const DrmDumbFramebuffer&) = delete;
    DrmDumbFramebuffer& operator=(const DrmDumbFramebuffer&) = delete;

    /**
     * @brief 在 XRGB8888 可见区域写入五条垂直测试色条。
     *
     * 写入顺序为红、绿、蓝、白、黑。函数先把包括 pitch padding 在内的完整映射
     * 清零，再逐像素写入，便于 checksum 稳定验证 mmap 确实可读写。
     *
     * @throws std::logic_error framebuffer 已经 release 或映射无效时抛出。
     */
    void fillColorBars();

    /**
     * @brief 对完整 mmap 区域计算 64-bit FNV-1a checksum。
     * @return 当前 buffer 内容的确定性 checksum。
     * @throws std::logic_error framebuffer 已经 release 或映射无效时抛出。
     */
    std::uint64_t checksum() const;

    /**
     * @brief 显式释放全部资源，并报告清理阶段的第一个错误。
     *
     * 释放顺序为 drmModeRmFB、munmap、DESTROY_DUMB。无论某一步是否失败，函数都会
     * 继续尝试后续清理并清除本地所有权标记。正常测试路径应显式调用本函数；析构
     * 仅作为不抛异常的兜底。
     *
     * @throws std::runtime_error 任一清理操作失败时抛出。
     */
    void release();

    /** @brief 获取 framebuffer 宽度，单位为像素。 */
    std::uint32_t width() const noexcept;

    /** @brief 获取 framebuffer 高度，单位为像素。 */
    std::uint32_t height() const noexcept;

    /** @brief 获取驱动返回的每行实际字节数。 */
    std::uint32_t pitch() const noexcept;

    /** @brief 获取驱动分配并 mmap 的总字节数。 */
    std::size_t size() const noexcept;

    /** @brief 获取当前 fd 命名空间中的 GEM handle。 */
    std::uint32_t handle() const noexcept;

    /** @brief 获取 drmModeAddFB2 创建的 framebuffer object ID。 */
    std::uint32_t framebufferId() const noexcept;

private:
    /**
     * @brief 释放当前已拥有的资源。
     * @param report_error true 时在全部清理完成后报告第一个错误；false 时不抛异常。
     */
    void releaseResources(bool report_error);

    /** 从 DrmDevice 借用且不得 close 的 fd。 */
    int drm_fd_{-1};

    /** framebuffer 可见宽度，单位为像素。 */
    std::uint32_t width_{0U};

    /** framebuffer 可见高度，单位为像素。 */
    std::uint32_t height_{0U};

    /** CREATE_DUMB 返回的实际每行字节数。 */
    std::uint32_t pitch_{0U};

    /** mmap 区域总长度，单位为字节。 */
    std::size_t size_{0U};

    /** 当前 DRM fd 拥有的 GEM handle；0 表示已释放。 */
    std::uint32_t handle_{0U};

    /** drmModeAddFB2 返回的 object ID；0 表示尚未创建或已释放。 */
    std::uint32_t framebuffer_id_{0U};

    /** mmap 返回的地址；nullptr 表示尚未映射或已释放。 */
    void* mapping_{nullptr};
};

/**
 * @brief 描述 DrmCrtcDisplay 结束显示时采取的恢复动作。
 */
enum class DrmCrtcRestoreResult {
    /** show() 尚未成功，退出时没有修改 CRTC。 */
    kNoDisplayChange,

    /** 测试结束后已经安全关闭目标 CRTC。 */
    kCrtcDisabled,
};

/**
 * @brief 独占一个 DRM CRTC，并显示指定 framebuffer。
 *
 * 构造函数先检查目标 CRTC 状态，再取得 DRM master 并查询完整 mode timing。默认
 * 拒绝覆盖 active CRTC；Rockchip BSP 在 Weston 正常退出后仍可能保留最后一帧，
 * 此时只有已在进程外确认桌面停止的调用方才能显式放行。show() 才会真正调用
 * drmModeSetCrtc。结束时应显式调用 restore()，先安全关闭 CRTC，再释放 master。
 *
 * 本类借用 DrmDevice 的 fd，不负责 close。它不可复制或移动，也不是线程安全的。
 * 默认探测和 dumb-buffer 生命周期测试不会构造本类。
 */
class DrmCrtcDisplay {
public:
    /**
     * @brief 取得 DRM master，并准备一个 connector/CRTC/mode 显示会话。
     *
     * @param drm_fd 从 DrmDevice 借用的 DRM primary node fd。
     * @param connector_id probe() 选择的 connected connector object ID。
     * @param crtc_id probe() 选择的 compatible CRTC object ID。
     * @param mode_name probe() 选择的 mode 名称，例如 `1080x1920`。
     * @param mode_width mode 的水平有效像素数。
     * @param mode_height mode 的垂直有效像素数。
     * @param allow_active_crtc true 表示调用方已在进程外确认 compositor 停止，
     * 允许覆盖驱动保留的最后一个 active framebuffer；false 时拒绝。
     * @throws std::invalid_argument fd、object ID 或 mode 尺寸无效时抛出。
     * @throws std::runtime_error 无法取得 DRM master、目标 CRTC 仍处于 active、
     * 查询资源或匹配完整 mode timing 时抛出。必须先停止 Weston 等显示服务。
     */
    DrmCrtcDisplay(int drm_fd,
                   std::uint32_t connector_id,
                   std::uint32_t crtc_id,
                   const std::string& mode_name,
                   std::uint32_t mode_width,
                   std::uint32_t mode_height,
                   bool allow_active_crtc);

    /** @brief best-effort 恢复/关闭 CRTC 并释放 DRM master。 */
    ~DrmCrtcDisplay();

    DrmCrtcDisplay(const DrmCrtcDisplay&) = delete;
    DrmCrtcDisplay& operator=(const DrmCrtcDisplay&) = delete;

    /**
     * @brief 将 framebuffer 绑定到目标 CRTC 和 connector，立即开始扫描输出。
     *
     * @param framebuffer_id 与目标 mode 尺寸一致的有效 KMS framebuffer ID。
     * @throws std::invalid_argument framebuffer_id 为 0 时抛出。
     * @throws std::logic_error 会话已恢复或 show() 已经成功调用时抛出。
     * @throws std::runtime_error drmModeSetCrtc() 失败时抛出。
     */
    void show(std::uint32_t framebuffer_id);

    /**
     * @brief 结束扫描输出并释放 DRM master。
     *
     * 测试结束时安全关闭目标 CRTC，等待桌面服务重新启动并自行 modeset。无论
     * 关闭是否成功，本函数都会尝试 drmDropMaster()。
     *
     * @return 实际执行的恢复动作。
     * @throws std::runtime_error 既不能恢复/关闭 CRTC，或者无法释放 master 时抛出。
     */
    DrmCrtcRestoreResult restore();

private:
    /** 隐藏 libdrm 原始结构体和会话状态，避免其进入公开接口。 */
    struct Impl;

    /** 独占实现对象；析构时执行不抛异常的兜底清理。 */
    std::unique_ptr<Impl> impl_;
};
