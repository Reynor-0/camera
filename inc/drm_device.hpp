#pragma once

#include <cstdint>
#include <string>

/**
 * @brief 保存 DRM connector 最终选中的显示模式。
 *
 * 该结构只保存后续业务需要的稳定值，不持有 libdrm 返回的指针。宽高是屏幕扫描
 * 方向上的像素数，刷新率根据 pixel clock 和 timing totals 计算。
 */
struct DrmModeInfo {
    /** DRM mode 名称，例如 `1080x1920`。 */
    std::string name;

    /** 水平显示区域宽度，单位为像素。 */
    std::uint32_t width{0U};

    /** 垂直显示区域高度，单位为像素。 */
    std::uint32_t height{0U};

    /** 根据完整 mode timing 计算的垂直刷新率，单位为 Hz。 */
    double refresh_rate_hz{0.0};

    /** true 表示 mode 带有 DRM_MODE_TYPE_PREFERRED 标志。 */
    bool preferred{false};
};

/**
 * @brief 保存一次 DRM/KMS 只读资源探测的归一化结果。
 *
 * connector、encoder 和 CRTC ID 由内核在当前启动周期分配，只用于日志和后续
 * KMS 初始化，不能写死到配置或源代码中。
 */
struct DrmProbeResult {
    /** drmGetVersion() 返回的内核 DRM driver 名称，例如 `rockchip`。 */
    std::string driver_name;

    /** 当前 DRM driver 是否支持 DRM_IOCTL_MODE_CREATE_DUMB。 */
    bool dumb_buffer_supported{false};

    /** connector 可读名称，例如 `DSI-1`。 */
    std::string connector_name;

    /** 已连接且具有可用 mode 的 connector object ID。 */
    std::uint32_t connector_id{0U};

    /** 与所选 connector 关联的 encoder object ID。 */
    std::uint32_t encoder_id{0U};

    /** encoder 当前或 possible_crtcs 指向的 CRTC object ID。 */
    std::uint32_t crtc_id{0U};

    /** connector 的 preferred mode；没有 preferred 标志时使用第一个 mode。 */
    DrmModeInfo mode;
};

/**
 * @brief 管理一个 DRM 设备 fd，并提供不改变显示状态的 KMS 资源探测。
 *
 * 本类拥有通过 open() 获得的 fd，并在析构时关闭。probe() 只调用查询接口，不申请
 * DRM master、不创建 framebuffer、不执行 modeset，因此允许在 Weston 运行时使用。
 * 本类不可复制或移动，也不是线程安全的。
 */
class DrmDevice {
public:
    /**
     * @brief 以读写和 close-on-exec 方式打开 DRM primary node。
     *
     * primary node 使用 O_RDWR 打开，为后续 KMS 阶段保持一致；本阶段不会执行任何
     * 修改显示状态的 ioctl。
     *
     * @param device_path DRM primary node 路径，通常为 `/dev/dri/card0`。
     * @throws std::invalid_argument 路径为空时抛出。
     * @throws std::runtime_error open() 失败时抛出，错误包含路径和 errno。
     */
    explicit DrmDevice(const std::string& device_path);

    /** @brief 关闭当前对象拥有的 DRM fd。 */
    ~DrmDevice();

    DrmDevice(const DrmDevice&) = delete;
    DrmDevice& operator=(const DrmDevice&) = delete;

    /**
     * @brief 自动选择 connected connector、preferred mode、encoder 和 CRTC。
     *
     * 选择顺序为 DRM resources 中的 connector 顺序；跳过未连接或没有 mode 的
     * connector。优先使用 connector 当前 encoder/CRTC，否则根据 possible_crtcs
     * 选择第一个兼容 CRTC。所有 libdrm 查询对象都会在返回前释放。
     *
     * @return 不持有 libdrm 指针的资源探测结果。
     * @throws std::runtime_error libdrm 查询失败、驱动不支持 KMS、没有 connected
     * connector，或找不到兼容 encoder/CRTC 时抛出。
     */
    DrmProbeResult probe() const;

    /**
     * @brief 获取当前 DRM 设备路径。
     * @return 构造时保存的路径引用，有效期与当前对象一致。
     */
    const std::string& path() const noexcept;

private:
    /** 当前对象拥有并负责 close 的 DRM fd。 */
    int fd_{-1};

    /** DRM node 路径副本，用于日志和错误上下文。 */
    std::string device_path_;
};
