#include "drm_device.hpp"

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

/** @brief 让 std::unique_ptr 调用 drmFreeVersion()。 */
struct DrmVersionDeleter {
    /**
     * @brief 释放 drmGetVersion() 返回的结构体。
     * @param version 可以为空的 libdrm version 指针。
     */
    void operator()(drmVersion* version) const noexcept
    {
        if (version != nullptr) {
            drmFreeVersion(version);
        }
    }
};

/** @brief 让 std::unique_ptr 调用 drmModeFreeResources()。 */
struct DrmResourcesDeleter {
    /**
     * @brief 释放 drmModeGetResources() 返回的结构体。
     * @param resources 可以为空的 KMS resources 指针。
     */
    void operator()(drmModeRes* resources) const noexcept
    {
        if (resources != nullptr) {
            drmModeFreeResources(resources);
        }
    }
};

/** @brief 让 std::unique_ptr 调用 drmModeFreeConnector()。 */
struct DrmConnectorDeleter {
    /**
     * @brief 释放 drmModeGetConnector() 返回的结构体。
     * @param connector 可以为空的 connector 指针。
     */
    void operator()(drmModeConnector* connector) const noexcept
    {
        if (connector != nullptr) {
            drmModeFreeConnector(connector);
        }
    }
};

/** @brief 让 std::unique_ptr 调用 drmModeFreeEncoder()。 */
struct DrmEncoderDeleter {
    /**
     * @brief 释放 drmModeGetEncoder() 返回的结构体。
     * @param encoder 可以为空的 encoder 指针。
     */
    void operator()(drmModeEncoder* encoder) const noexcept
    {
        if (encoder != nullptr) {
            drmModeFreeEncoder(encoder);
        }
    }
};

typedef std::unique_ptr<drmVersion, DrmVersionDeleter> DrmVersionOwner;
typedef std::unique_ptr<drmModeRes, DrmResourcesDeleter> DrmResourcesOwner;
typedef std::unique_ptr<drmModeConnector, DrmConnectorDeleter>
    DrmConnectorOwner;
typedef std::unique_ptr<drmModeEncoder, DrmEncoderDeleter> DrmEncoderOwner;

/**
 * @brief 将 errno 转换为包含操作和设备上下文的异常。
 * @param operation 失败的 POSIX 或 libdrm 操作名称。
 * @param device_path 发生错误的 DRM node 路径。
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

/**
 * @brief 判断 object ID 是否属于当前 DRM resources 的 CRTC 列表。
 * @param resources drmModeGetResources() 返回的只读资源表。
 * @param crtc_id 待检查的 CRTC object ID。
 * @return ID 位于 resources.crtcs[] 中时返回 true。
 */
bool containsCrtc(const drmModeRes& resources, std::uint32_t crtc_id)
{
    for (int index = 0; index < resources.count_crtcs; ++index) {
        if (resources.crtcs[index] == crtc_id) {
            return true;
        }
    }
    return false;
}

/**
 * @brief 从一个 encoder 选择当前或第一个 possible CRTC。
 * @param resources DRM resources，提供 CRTC ID 数组及其 bit 下标。
 * @param encoder 待检查的 encoder。
 * @param crtc_id 非空输出参数；成功时写入选中的 CRTC object ID。
 * @return 找到合法 CRTC 时返回 true，否则返回 false。
 */
bool selectCrtc(const drmModeRes& resources,
                const drmModeEncoder& encoder,
                std::uint32_t* crtc_id)
{
    if (encoder.crtc_id != 0U && containsCrtc(resources, encoder.crtc_id)) {
        *crtc_id = encoder.crtc_id;
        return true;
    }

    for (int index = 0; index < resources.count_crtcs; ++index) {
        // possible_crtcs 的 bit N 对应 resources.crtcs[N]，不是 CRTC object ID。
        if (index < 32 &&
            (encoder.possible_crtcs & (1U << static_cast<unsigned int>(index))) !=
                0U) {
            *crtc_id = resources.crtcs[index];
            return true;
        }
    }
    return false;
}

/**
 * @brief 为 connector 选择 encoder 及其兼容 CRTC。
 * @param fd 已打开的 DRM fd。
 * @param resources 当前 DRM resources。
 * @param connector 已连接且具有 mode 的 connector。
 * @param encoder_id 非空输出参数；成功时写入 encoder object ID。
 * @param crtc_id 非空输出参数；成功时写入 CRTC object ID。
 * @return 找到完整 encoder/CRTC 组合时返回 true，否则返回 false。
 */
bool selectEncoderAndCrtc(int fd,
                          const drmModeRes& resources,
                          const drmModeConnector& connector,
                          std::uint32_t* encoder_id,
                          std::uint32_t* crtc_id)
{
    // 当前 encoder 最能反映内核或 compositor 已建立的 connector 路由，优先使用。
    if (connector.encoder_id != 0U) {
        DrmEncoderOwner current(drmModeGetEncoder(fd, connector.encoder_id));
        if (current && selectCrtc(resources, *current, crtc_id)) {
            *encoder_id = current->encoder_id;
            return true;
        }
    }

    // connector 未启用时 encoder_id 可能为 0，此时枚举 encoders[] 并根据
    // possible_crtcs bitmask 选择第一组可行路由。
    for (int index = 0; index < connector.count_encoders; ++index) {
        const std::uint32_t candidate_id = connector.encoders[index];
        if (candidate_id == connector.encoder_id) {
            continue;
        }
        DrmEncoderOwner candidate(drmModeGetEncoder(fd, candidate_id));
        if (candidate && selectCrtc(resources, *candidate, crtc_id)) {
            *encoder_id = candidate->encoder_id;
            return true;
        }
    }
    return false;
}

/**
 * @brief 选择 connector 的 preferred mode，缺少 preferred 标志时选择第一个。
 * @param connector 已连接且 count_modes 大于 0 的 connector。
 * @return 指向 connector.modes[] 内部元素的非拥有指针。
 */
const drmModeModeInfo* selectMode(const drmModeConnector& connector)
{
    for (int index = 0; index < connector.count_modes; ++index) {
        if ((connector.modes[index].type & DRM_MODE_TYPE_PREFERRED) != 0U) {
            return &connector.modes[index];
        }
    }
    return &connector.modes[0];
}

/**
 * @brief 根据 DRM mode timing 计算实际垂直刷新率。
 * @param mode drmModeModeInfo 中的 pixel clock 和完整 timing。
 * @return 刷新率，单位 Hz；timing 不完整时返回 0。
 */
double calculateRefreshRate(const drmModeModeInfo& mode)
{
    if (mode.htotal == 0U || mode.vtotal == 0U) {
        return 0.0;
    }

    // mode.clock 的单位是 kHz，乘 1000 后除以一帧的总像素时钟数得到 Hz。
    double refresh_rate =
        static_cast<double>(mode.clock) * 1000.0 /
        (static_cast<double>(mode.htotal) * static_cast<double>(mode.vtotal));
    if ((mode.flags & DRM_MODE_FLAG_INTERLACE) != 0U) {
        refresh_rate *= 2.0;
    }
    if ((mode.flags & DRM_MODE_FLAG_DBLSCAN) != 0U) {
        refresh_rate /= 2.0;
    }
    if (mode.vscan > 1U) {
        refresh_rate /= static_cast<double>(mode.vscan);
    }
    return refresh_rate;
}

/**
 * @brief 将 DRM_MODE_CONNECTOR_* 类型转换为稳定的可读名称。
 * @param connector_type drmModeConnector.connector_type 数值。
 * @return 与内核 UAPI 名称对应的字符串；未知类型返回 `Unknown`。
 */
const char* connectorTypeName(std::uint32_t connector_type)
{
    // ATK BSP 自带的旧版 libdrm 尚未提供 drmModeGetConnectorTypeName()，因此直接
    // 映射内核 UAPI 常量。这样不会引入新符号，也不要求升级目标板动态库。
    switch (connector_type) {
        case DRM_MODE_CONNECTOR_VGA:
            return "VGA";
        case DRM_MODE_CONNECTOR_DVII:
            return "DVI-I";
        case DRM_MODE_CONNECTOR_DVID:
            return "DVI-D";
        case DRM_MODE_CONNECTOR_DVIA:
            return "DVI-A";
        case DRM_MODE_CONNECTOR_Composite:
            return "Composite";
        case DRM_MODE_CONNECTOR_SVIDEO:
            return "SVIDEO";
        case DRM_MODE_CONNECTOR_LVDS:
            return "LVDS";
        case DRM_MODE_CONNECTOR_Component:
            return "Component";
        case DRM_MODE_CONNECTOR_9PinDIN:
            return "DIN";
        case DRM_MODE_CONNECTOR_DisplayPort:
            return "DP";
        case DRM_MODE_CONNECTOR_HDMIA:
            return "HDMI-A";
        case DRM_MODE_CONNECTOR_HDMIB:
            return "HDMI-B";
        case DRM_MODE_CONNECTOR_TV:
            return "TV";
        case DRM_MODE_CONNECTOR_eDP:
            return "eDP";
        case DRM_MODE_CONNECTOR_VIRTUAL:
            return "Virtual";
        case DRM_MODE_CONNECTOR_DSI:
            return "DSI";
        case DRM_MODE_CONNECTOR_DPI:
            return "DPI";
        default:
            return "Unknown";
    }
}

/**
 * @brief 生成与 modetest 一致的 connector 可读名称。
 * @param connector libdrm connector 结构体。
 * @return `类型-序号` 字符串，例如 `DSI-1`。
 */
std::string connectorName(const drmModeConnector& connector)
{
    return std::string(connectorTypeName(connector.connector_type)) + "-" +
           std::to_string(connector.connector_type_id);
}

}  // namespace

DrmDevice::DrmDevice(const std::string& device_path)
    : device_path_(device_path)
{
    if (device_path_.empty()) {
        throw std::invalid_argument("DRM device path must not be empty");
    }

    fd_ = ::open(device_path_.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        throw systemError("open", device_path_);
    }
}

DrmDevice::~DrmDevice()
{
    if (fd_ >= 0) {
        static_cast<void>(::close(fd_));
        fd_ = -1;
    }
}

DrmProbeResult DrmDevice::probe() const
{
    DrmProbeResult result;

    DrmVersionOwner version(drmGetVersion(fd_));
    if (!version) {
        throw systemError("drmGetVersion", device_path_);
    }
    if (version->name == nullptr || version->name_len <= 0) {
        throw std::runtime_error("drmGetVersion returned an empty driver name for " +
                                 device_path_);
    }
    result.driver_name = std::string(
        version->name, static_cast<std::size_t>(version->name_len));

    std::uint64_t dumb_buffer_capability = 0U;
    if (drmGetCap(fd_, DRM_CAP_DUMB_BUFFER, &dumb_buffer_capability) != 0) {
        throw systemError("drmGetCap(DRM_CAP_DUMB_BUFFER)", device_path_);
    }
    result.dumb_buffer_supported = dumb_buffer_capability != 0U;

    DrmResourcesOwner resources(drmModeGetResources(fd_));
    if (!resources) {
        throw systemError("drmModeGetResources", device_path_);
    }
    if (resources->count_connectors <= 0 || resources->count_crtcs <= 0) {
        throw std::runtime_error("DRM device exposes no connector or CRTC resources: " +
                                 device_path_);
    }

    for (int index = 0; index < resources->count_connectors; ++index) {
        DrmConnectorOwner connector(
            drmModeGetConnector(fd_, resources->connectors[index]));
        if (!connector || connector->connection != DRM_MODE_CONNECTED ||
            connector->count_modes <= 0) {
            continue;
        }

        std::uint32_t encoder_id = 0U;
        std::uint32_t crtc_id = 0U;
        if (!selectEncoderAndCrtc(fd_,
                                  *resources,
                                  *connector,
                                  &encoder_id,
                                  &crtc_id)) {
            continue;
        }

        const drmModeModeInfo* const mode = selectMode(*connector);
        result.connector_name = connectorName(*connector);
        result.connector_id = connector->connector_id;
        result.encoder_id = encoder_id;
        result.crtc_id = crtc_id;
        result.mode.name = mode->name;
        result.mode.width = mode->hdisplay;
        result.mode.height = mode->vdisplay;
        result.mode.refresh_rate_hz = calculateRefreshRate(*mode);
        result.mode.preferred =
            (mode->type & DRM_MODE_TYPE_PREFERRED) != 0U;
        return result;
    }

    throw std::runtime_error(
        "no connected DRM connector with a usable mode/CRTC was found for " +
        device_path_);
}

const std::string& DrmDevice::path() const noexcept
{
    return device_path_;
}
