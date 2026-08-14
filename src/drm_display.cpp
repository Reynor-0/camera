#include "drm_display.hpp"

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <drm_fourcc.h>
#include <drm_mode.h>

#include <sys/mman.h>
#include <sys/types.h>

#include <cerrno>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

/**
 * @brief 构造包含 errno 的 DRM framebuffer 异常。
 * @param operation 失败的 ioctl、libdrm 或 POSIX 操作名称。
 * @param error errno 数值，必须在其他清理调用改变 errno 前保存。
 * @return 可直接抛出的 std::runtime_error。
 */
std::runtime_error framebufferError(const std::string& operation, int error)
{
    return std::runtime_error(operation + " failed: " + std::strerror(error) +
                              " (errno=" + std::to_string(error) + ")");
}

/** @brief 让 std::unique_ptr 调用 drmModeFreeConnector()。 */
struct DrmConnectorDeleter {
    /** @param connector 可以为空的 DRM connector 指针。 */
    void operator()(drmModeConnector* connector) const noexcept
    {
        if (connector != nullptr) {
            drmModeFreeConnector(connector);
        }
    }
};

/** @brief 让 std::unique_ptr 调用 drmModeFreeCrtc()。 */
struct DrmCrtcDeleter {
    /** @param crtc 可以为空的 DRM CRTC 指针。 */
    void operator()(drmModeCrtc* crtc) const noexcept
    {
        if (crtc != nullptr) {
            drmModeFreeCrtc(crtc);
        }
    }
};

typedef std::unique_ptr<drmModeConnector, DrmConnectorDeleter>
    DrmConnectorOwner;
typedef std::unique_ptr<drmModeCrtc, DrmCrtcDeleter> DrmCrtcOwner;

}  // namespace

DrmDumbFramebuffer::DrmDumbFramebuffer(int drm_fd,
                                       std::uint32_t width,
                                       std::uint32_t height)
    : drm_fd_(drm_fd),
      width_(width),
      height_(height)
{
    if (drm_fd_ < 0) {
        throw std::invalid_argument(
            "DrmDumbFramebuffer requires a valid DRM fd");
    }
    if (width_ == 0U || height_ == 0U) {
        throw std::invalid_argument(
            "DrmDumbFramebuffer width and height must be greater than zero");
    }

    try {
        // CREATE_DUMB 让 DRM driver 分配一个线性 GEM buffer。bpp=32 对应每个
        // XRGB8888 像素占 4 字节；驱动回填 handle、pitch 和 size。
        drm_mode_create_dumb create{};
        create.width = width_;
        create.height = height_;
        create.bpp = 32U;
        if (drmIoctl(drm_fd_, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
            throw framebufferError("DRM_IOCTL_MODE_CREATE_DUMB", errno);
        }

        // ioctl 成功后立即接管 handle。这样即使后续发现 pitch/size 异常，catch
        // 分支仍能调用 DESTROY_DUMB，避免构造失败路径遗留 GEM 对象。
        handle_ = create.handle;
        if (create.handle == 0U || create.pitch == 0U || create.size == 0U) {
            throw std::runtime_error(
                "DRM_IOCTL_MODE_CREATE_DUMB returned invalid buffer metadata");
        }
        if (create.size >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
            throw std::runtime_error(
                "DRM dumb buffer is too large for this process address space");
        }

        pitch_ = create.pitch;
        size_ = static_cast<std::size_t>(create.size);

        const std::uint64_t visible_row_bytes =
            static_cast<std::uint64_t>(width_) * sizeof(std::uint32_t);
        const std::uint64_t required_size =
            static_cast<std::uint64_t>(pitch_) *
            static_cast<std::uint64_t>(height_);
        if (visible_row_bytes > static_cast<std::uint64_t>(pitch_) ||
            required_size > create.size ||
            (pitch_ % sizeof(std::uint32_t)) != 0U) {
            throw std::runtime_error(
                "DRM_IOCTL_MODE_CREATE_DUMB returned an unsafe buffer layout");
        }

        // MAP_DUMB 不直接建立用户态映射，只返回随后传给 mmap 的 DRM offset。
        drm_mode_map_dumb map_request{};
        map_request.handle = handle_;
        if (drmIoctl(drm_fd_, DRM_IOCTL_MODE_MAP_DUMB, &map_request) != 0) {
            throw framebufferError("DRM_IOCTL_MODE_MAP_DUMB", errno);
        }
        if (map_request.offset >
            static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
            throw std::runtime_error(
                "DRM dumb mmap offset cannot be represented by off_t");
        }

        mapping_ = ::mmap(nullptr,
                          size_,
                          PROT_READ | PROT_WRITE,
                          MAP_SHARED,
                          drm_fd_,
                          static_cast<off_t>(map_request.offset));
        if (mapping_ == MAP_FAILED) {
            mapping_ = nullptr;
            throw framebufferError("mmap(DRM dumb buffer)", errno);
        }

        // AddFB2 只为 GEM buffer 创建 KMS framebuffer 元数据。本阶段不把该 ID
        // 绑定到 plane/CRTC，因此不会修改 Weston 正在显示的 framebuffer。
        std::uint32_t handles[4]{};
        std::uint32_t pitches[4]{};
        std::uint32_t offsets[4]{};
        handles[0] = handle_;
        pitches[0] = pitch_;
        offsets[0] = 0U;
        if (drmModeAddFB2(drm_fd_,
                          width_,
                          height_,
                          DRM_FORMAT_XRGB8888,
                          handles,
                          pitches,
                          offsets,
                          &framebuffer_id_,
                          0U) != 0) {
            throw framebufferError("drmModeAddFB2(XRGB8888)", errno);
        }
        if (framebuffer_id_ == 0U) {
            throw std::runtime_error(
                "drmModeAddFB2 returned an invalid framebuffer ID");
        }
    } catch (...) {
        releaseResources(false);
        throw;
    }
}

DrmDumbFramebuffer::~DrmDumbFramebuffer()
{
    releaseResources(false);
}

void DrmDumbFramebuffer::fillColorBars()
{
    if (mapping_ == nullptr || size_ == 0U || framebuffer_id_ == 0U) {
        throw std::logic_error(
            "fillColorBars requires a valid DRM dumb framebuffer");
    }

    static const std::uint32_t kColors[] = {
        0x00ff0000U,  // 红色：XRGB8888 中 R=255。
        0x0000ff00U,  // 绿色：XRGB8888 中 G=255。
        0x000000ffU,  // 蓝色：XRGB8888 中 B=255。
        0x00ffffffU,  // 白色：R/G/B 均为 255。
        0x00000000U,  // 黑色：R/G/B 均为 0。
    };
    const std::size_t color_count = sizeof(kColors) / sizeof(kColors[0]);

    // 先清理驱动可能加入的 pitch padding，使完整映射 checksum 可重复。
    std::memset(mapping_, 0, size_);
    unsigned char* const base = static_cast<unsigned char*>(mapping_);
    for (std::uint32_t y = 0U; y < height_; ++y) {
        std::uint32_t* const row = reinterpret_cast<std::uint32_t*>(
            base + static_cast<std::size_t>(y) *
                       static_cast<std::size_t>(pitch_));
        for (std::uint32_t x = 0U; x < width_; ++x) {
            const std::size_t color_index =
                static_cast<std::size_t>(x) * color_count /
                static_cast<std::size_t>(width_);
            row[x] = kColors[color_index];
        }
    }
}

std::uint64_t DrmDumbFramebuffer::checksum() const
{
    if (mapping_ == nullptr || size_ == 0U || framebuffer_id_ == 0U) {
        throw std::logic_error(
            "checksum requires a valid DRM dumb framebuffer");
    }

    // FNV-1a 仅用于发现 mmap 写入/读取或布局异常，不作为密码学完整性校验。
    const unsigned char* const bytes =
        static_cast<const unsigned char*>(mapping_);
    std::uint64_t hash = 14695981039346656037ULL;
    for (std::size_t index = 0U; index < size_; ++index) {
        hash ^= static_cast<std::uint64_t>(bytes[index]);
        hash *= 1099511628211ULL;
    }
    return hash;
}

void DrmDumbFramebuffer::release()
{
    releaseResources(true);
}

std::uint32_t DrmDumbFramebuffer::width() const noexcept
{
    return width_;
}

std::uint32_t DrmDumbFramebuffer::height() const noexcept
{
    return height_;
}

std::uint32_t DrmDumbFramebuffer::pitch() const noexcept
{
    return pitch_;
}

std::size_t DrmDumbFramebuffer::size() const noexcept
{
    return size_;
}

std::uint32_t DrmDumbFramebuffer::handle() const noexcept
{
    return handle_;
}

std::uint32_t DrmDumbFramebuffer::framebufferId() const noexcept
{
    return framebuffer_id_;
}

void DrmDumbFramebuffer::releaseResources(bool report_error)
{
    int first_error = 0;
    const char* first_operation = nullptr;

    if (framebuffer_id_ != 0U) {
        if (drmModeRmFB(drm_fd_, framebuffer_id_) != 0 &&
            first_error == 0) {
            first_error = errno;
            first_operation = "drmModeRmFB";
        }
        framebuffer_id_ = 0U;
    }

    if (mapping_ != nullptr && size_ > 0U) {
        if (::munmap(mapping_, size_) != 0 && first_error == 0) {
            first_error = errno;
            first_operation = "munmap(DRM dumb buffer)";
        }
        mapping_ = nullptr;
    }

    if (handle_ != 0U) {
        drm_mode_destroy_dumb destroy{};
        destroy.handle = handle_;
        if (drmIoctl(drm_fd_, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy) != 0 &&
            first_error == 0) {
            first_error = errno;
            first_operation = "DRM_IOCTL_MODE_DESTROY_DUMB";
        }
        handle_ = 0U;
    }
    pitch_ = 0U;
    size_ = 0U;

    if (report_error && first_error != 0) {
        throw framebufferError(first_operation, first_error);
    }
}

/** @brief 保存独占 CRTC 会话需要的 libdrm 对象和状态。 */
struct DrmCrtcDisplay::Impl {
    /**
     * @brief 检查初始 CRTC、解析目标 mode timing，并取得 master。
     * @param drm_fd 借用的 DRM primary node fd。
     * @param connector_id 目标 connector object ID。
     * @param crtc_id 目标 CRTC object ID。
     * @param mode_name 目标 mode 名称。
     * @param mode_width 目标 mode 水平有效像素数。
     * @param mode_height 目标 mode 垂直有效像素数。
     * @param allow_active_crtc 是否允许覆盖 compositor 退出后保留的 active CRTC。
     */
    Impl(int drm_fd,
         std::uint32_t connector_id,
         std::uint32_t crtc_id,
         const std::string& mode_name,
         std::uint32_t mode_width,
         std::uint32_t mode_height,
         bool allow_active_crtc)
        : drm_fd(drm_fd),
          connector_id(connector_id),
          crtc_id(crtc_id)
    {
        if (drm_fd < 0 || connector_id == 0U || crtc_id == 0U ||
            mode_name.empty() || mode_width == 0U || mode_height == 0U) {
            throw std::invalid_argument(
                "DrmCrtcDisplay requires valid fd, object IDs and mode");
        }

        try {
            DrmConnectorOwner target_connector(
                drmModeGetConnector(drm_fd, connector_id));
            if (!target_connector) {
                throw framebufferError("drmModeGetConnector(target)", errno);
            }
            if (target_connector->connection != DRM_MODE_CONNECTED) {
                throw std::runtime_error(
                    "target DRM connector is no longer connected");
            }

            bool mode_found = false;
            for (int index = 0; index < target_connector->count_modes;
                 ++index) {
                const drmModeModeInfo& candidate =
                    target_connector->modes[index];
                if (candidate.hdisplay == mode_width &&
                    candidate.vdisplay == mode_height &&
                    mode_name == candidate.name) {
                    target_mode = candidate;
                    mode_found = true;
                    break;
                }
            }
            if (!mode_found) {
                throw std::runtime_error(
                    "selected DRM mode disappeared before modeset");
            }

            DrmCrtcOwner initial_crtc(drmModeGetCrtc(drm_fd, crtc_id));
            if (!initial_crtc) {
                throw framebufferError("drmModeGetCrtc(initial)", errno);
            }
            if (!allow_active_crtc &&
                (initial_crtc->mode_valid != 0 ||
                 initial_crtc->buffer_id != 0U)) {
                // Rockchip 4.19 BSP 实测可能在 Weston 仍标记为 master 时允许 root
                // 客户端 legacy modeset。要求 CRTC 事先为 inactive，可在真正覆盖
                // 桌面 framebuffer 前形成第二道、可观察的安全边界。
                throw std::runtime_error(
                    "target CRTC is still active; stop Weston before color-bar modeset");
            }

            // 必须在确认 CRTC inactive 后才触碰 master。Rockchip 4.19 BSP 实测中，
            // 对 active Weston 会话调用 drmSetMaster 可能让 compositor 退出，即使
            // 应用随后没有执行 modeset。
            if (drmIsMaster(drm_fd) != 1 && drmSetMaster(drm_fd) != 0) {
                const int error = errno;
                throw framebufferError(
                    "drmSetMaster (stop Weston before display takeover)",
                    error);
            }
            if (drmIsMaster(drm_fd) != 1) {
                throw std::runtime_error(
                    "DRM fd is not master after drmSetMaster");
            }
            master_held = true;
        } catch (...) {
            static_cast<void>(release(false));
            throw;
        }
    }

    /** @brief 不抛异常地执行最后一次恢复和 master 释放。 */
    ~Impl()
    {
        static_cast<void>(release(false));
    }

    /**
     * @brief 将 framebuffer 绑定到构造时选中的 CRTC。
     * @param framebuffer_id 有效的 KMS framebuffer object ID。
     */
    void show(std::uint32_t framebuffer_id)
    {
        if (framebuffer_id == 0U) {
            throw std::invalid_argument(
                "DrmCrtcDisplay::show requires a framebuffer ID");
        }
        if (released) {
            throw std::logic_error(
                "DrmCrtcDisplay session has already been restored");
        }
        if (display_active) {
            throw std::logic_error(
                "DrmCrtcDisplay::show may only be called once");
        }

        // legacy SetCrtc 同时完成 mode、connector 路由和 primary framebuffer
        // 设置。只有成功返回后，display_active 才能变为 true。
        std::uint32_t connector = connector_id;
        if (drmModeSetCrtc(drm_fd,
                           crtc_id,
                           framebuffer_id,
                           0U,
                           0U,
                           &connector,
                           1,
                           &target_mode) != 0) {
            throw framebufferError("drmModeSetCrtc(color bars)", errno);
        }
        display_active = true;
    }

    /**
     * @brief 恢复/关闭 CRTC，并释放 DRM master。
     * @param report_error true 时报告无法形成安全状态或 drop master 的错误。
     * @return 实际恢复动作。
     */
    DrmCrtcRestoreResult release(bool report_error)
    {
        if (released) {
            return restore_result;
        }

        int safety_error = 0;
        int drop_master_error = 0;
        if (display_active) {
            // 不能在测试 framebuffer 仍被 CRTC 扫描时删除 framebuffer/GEM，
            // 因此退出的第一步固定为关闭 CRTC。
            if (drmModeSetCrtc(drm_fd,
                               crtc_id,
                               0U,
                               0U,
                               0U,
                               nullptr,
                               0,
                               nullptr) != 0) {
                safety_error = errno;
            } else {
                restore_result = DrmCrtcRestoreResult::kCrtcDisabled;
            }
            display_active = false;
        }

        if (master_held) {
            if (drmDropMaster(drm_fd) != 0) {
                const int error = errno;
                // 厂商 BSP 中另一个 compositor 可能已经重新成为 master，此时本 fd
                // 的 drmDropMaster 返回 EINVAL/权限错误。close(fd) 仍会释放本 fd 的
                // file-local 状态，这三种结果不代表遗留了 master 所有权。
                if (error != EINVAL && error != EACCES && error != EPERM) {
                    drop_master_error = error;
                }
            }
            master_held = false;
        }
        released = true;

        if (report_error && safety_error != 0) {
            throw framebufferError(
                "drmModeSetCrtc(restore or disable)", safety_error);
        }
        if (report_error && drop_master_error != 0) {
            throw framebufferError("drmDropMaster", drop_master_error);
        }
        return restore_result;
    }

    /** 借用且不得 close 的 DRM fd。 */
    int drm_fd{-1};

    /** 本次 modeset 使用的 connector object ID。 */
    std::uint32_t connector_id{0U};

    /** 本次 modeset 使用的 CRTC object ID。 */
    std::uint32_t crtc_id{0U};

    /** 从 connector 重新查询得到的完整硬件 timing。 */
    drmModeModeInfo target_mode{};

    /** true 表示当前 fd 尚持有 DRM master。 */
    bool master_held{false};

    /** true 表示测试 framebuffer 已经成功绑定到 CRTC。 */
    bool display_active{false};

    /** true 表示 release() 已经执行，不得再次 modeset。 */
    bool released{false};

    /** 保存幂等 release() 应重复返回的结果。 */
    DrmCrtcRestoreResult restore_result{
        DrmCrtcRestoreResult::kNoDisplayChange};
};

DrmCrtcDisplay::DrmCrtcDisplay(int drm_fd,
                               std::uint32_t connector_id,
                               std::uint32_t crtc_id,
                               const std::string& mode_name,
                               std::uint32_t mode_width,
                               std::uint32_t mode_height,
                               bool allow_active_crtc)
    : impl_(new Impl(drm_fd,
                     connector_id,
                     crtc_id,
                     mode_name,
                     mode_width,
                     mode_height,
                     allow_active_crtc))
{
}

DrmCrtcDisplay::~DrmCrtcDisplay() = default;

void DrmCrtcDisplay::show(std::uint32_t framebuffer_id)
{
    impl_->show(framebuffer_id);
}

DrmCrtcRestoreResult DrmCrtcDisplay::restore()
{
    return impl_->release(true);
}
