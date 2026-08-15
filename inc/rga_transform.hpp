#pragma once

#include <cstdint>
#include <string>

/** @brief 保存一次同步 RGA 转换的诊断结果。 */
struct RgaTransformResult {
    /** librga/RGA 报告的版本字符串。 */
    std::string version;

    /** 从提交到同步完成的用户态测量时间，单位为微秒。 */
    std::uint64_t elapsed_microseconds{0U};
};

/**
 * @brief 使用 RGA 将 NV12 DMA-BUF 旋转 270°并转换到 BGRX8888 DMA-BUF。
 *
 * BGRX8888 的字节布局与 little-endian DRM_FORMAT_XRGB8888 framebuffer 匹配。
 * 函数使用同步 IM2D 作业，成功返回时源 buffer 已不再被 RGA 读取，目标 buffer
 * 也已经可以交给 VOP 扫描。
 *
 * @param source_fd NV12 源 DMA-BUF fd。
 * @param source_width 有效源宽度。
 * @param source_height 有效源高度。
 * @param source_stride_pixels 源水平 stride；NV12 8-bit 下等于字节 stride。
 * @param source_height_stride 源 luma 高度 stride。
 * @param destination_fd BGRX8888 目标 DMA-BUF fd。
 * @param destination_width 有效目标宽度，必须等于 source_height。
 * @param destination_height 有效目标高度，必须等于 source_width。
 * @param destination_stride_pixels 目标水平 stride，单位为像素。
 * @param destination_height_stride 目标高度 stride。
 * @return RGA 版本和同步处理耗时。
 * @throws std::invalid_argument 尺寸、stride 或 fd 无效时抛出。
 * @throws std::runtime_error RGA 参数检查或硬件作业失败时抛出。
 */
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
    std::uint32_t destination_height_stride);
