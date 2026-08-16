#include "rga_transform.hpp"

#include <im2d.h>
#include <rga.h>

#include <chrono>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

/** @brief 将经过范围检查的 uint32_t 转换为旧版 IM2D 使用的 int。 */
int checkedInt(std::uint32_t value, const char* name)
{
    if (value == 0U ||
        value > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument(std::string(name) +
                                    " is outside the positive int range");
    }
    return static_cast<int>(value);
}

/** @brief 将 IM2D 状态码转换为带上下文的异常。 */
std::runtime_error rgaError(const char* operation, IM_STATUS status)
{
    const char* const detail = imStrError_t(status);
    return std::runtime_error(std::string(operation) + " failed: " +
                              (detail != nullptr ? detail : "unknown RGA error") +
                              " (status=" +
                              std::to_string(static_cast<int>(status)) + ")");
}

}  // namespace

RgaTransformResult rotateNv12ToBgrx8888(
    int source_fd,
    std::uint32_t source_width,
    std::uint32_t source_height,
    std::uint32_t source_stride_pixels,
    std::uint32_t source_height_stride,
    int destination_fd,
    std::uint32_t destination_width,
    std::uint32_t destination_height,
    std::uint32_t destination_stride_pixels,
    std::uint32_t destination_height_stride,
    RgaYuvToRgbMode color_mode)
{
    if (source_fd < 0 || destination_fd < 0) {
        throw std::invalid_argument(
            "RGA source and destination DMA-BUF fds must be valid");
    }
    if (destination_width != source_height ||
        destination_height != source_width) {
        throw std::invalid_argument(
            "270-degree RGA destination dimensions must swap source width and height");
    }
    if (source_stride_pixels < source_width ||
        source_height_stride < source_height ||
        destination_stride_pixels < destination_width ||
        destination_height_stride < destination_height ||
        (source_width % 2U) != 0U || (source_height % 2U) != 0U ||
        (source_stride_pixels % 2U) != 0U ||
        (source_height_stride % 2U) != 0U) {
        throw std::invalid_argument(
            "RGA NV12 dimensions and strides are invalid or not 2-pixel aligned");
    }

    rga_buffer_t source = wrapbuffer_fd_t(
        source_fd,
        checkedInt(source_width, "source width"),
        checkedInt(source_height, "source height"),
        checkedInt(source_stride_pixels, "source stride"),
        checkedInt(source_height_stride, "source height stride"),
        RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t destination = wrapbuffer_fd_t(
        destination_fd,
        checkedInt(destination_width, "destination width"),
        checkedInt(destination_height, "destination height"),
        checkedInt(destination_stride_pixels, "destination stride"),
        checkedInt(destination_height_stride, "destination height stride"),
        RK_FORMAT_BGRX_8888);

    // 板端 librga 1.3.1/so 2.1.0 只提供 BT.601 limited、BT.601 full 和
    // BT.709 limited 三种旧式 YUV->RGB CSC。调用方必须显式选择；这里不能根据
    // NV12 fourcc 猜测矩阵或量化范围。
    switch (color_mode) {
        case RgaYuvToRgbMode::Bt601Limited:
            source.color_space_mode = IM_YUV_TO_RGB_BT601_LIMIT;
            break;
        case RgaYuvToRgbMode::Bt601Full:
            source.color_space_mode = IM_YUV_TO_RGB_BT601_FULL;
            break;
        case RgaYuvToRgbMode::Bt709Limited:
            source.color_space_mode = IM_YUV_TO_RGB_BT709_LIMIT;
            break;
    }
    destination.color_space_mode = IM_COLOR_SPACE_DEFAULT;

    im_rect source_rect{};
    source_rect.width = checkedInt(source_width, "source rect width");
    source_rect.height = checkedInt(source_height, "source rect height");
    im_rect destination_rect{};
    destination_rect.width =
        checkedInt(destination_width, "destination rect width");
    destination_rect.height =
        checkedInt(destination_height, "destination rect height");
    im_rect pattern_rect{};
    rga_buffer_t pattern{};

    const int usage = IM_HAL_TRANSFORM_ROT_270 | IM_SYNC;
    rga_check_perpare(&source,
                      &destination,
                      &pattern,
                      &source_rect,
                      &destination_rect,
                      &pattern_rect,
                      usage);
    const IM_STATUS check_status = imcheck_t(source,
                                             destination,
                                             pattern,
                                             source_rect,
                                             destination_rect,
                                             pattern_rect,
                                             usage);
    if (check_status != IM_STATUS_NOERROR) {
        throw rgaError("imcheck_t(NV12->BGRX rotate 270)", check_status);
    }

    const std::chrono::steady_clock::time_point start =
        std::chrono::steady_clock::now();
    const IM_STATUS process_status = improcess(source,
                                               destination,
                                               pattern,
                                               source_rect,
                                               destination_rect,
                                               pattern_rect,
                                               usage);
    const std::chrono::steady_clock::time_point end =
        std::chrono::steady_clock::now();
    if (process_status != IM_STATUS_SUCCESS) {
        throw rgaError("improcess(NV12->BGRX rotate 270)", process_status);
    }

    RgaTransformResult result;
    const char* const version = querystring(RGA_VERSION);
    result.version = version != nullptr ? version : "unknown";
    result.elapsed_microseconds =
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(end - start)
                .count());
    return result;
}
