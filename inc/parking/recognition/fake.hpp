/**
 * @file fake.hpp
 * @brief 定义无需 Camera 或模型的固定结果识别器。
 */
#pragma once

#include "parking/recognition/recognizer.hpp"

#include <cstddef>

namespace parking {

/**
 * @brief 返回可配置固定结果的测试车牌识别 adapter。
 *
 * 该类不访问 Camera 或模型，只用于主机单元测试和后续 IPC 联调前的业务闭环。
 */
class FakePlateRecognizer : public PlateRecognizer {
public:
    /** @brief 创建默认返回 `TEST0001`、置信度 900000 的成功识别器。 */
    FakePlateRecognizer();

    /**
     * @brief 设置后续每次调用返回的固定结果。
     * @param result 后续识别调用使用的结果。
     */
    void setResult(const PlateRecognitionResult& result);

    /**
     * @brief 返回固定结果并增加调用计数。
     * @param event 当前入场事件；fake adapter 不读取图像，仅保留接口语义。
     * @return 通过 `setResult()` 配置的结果。
     */
    PlateRecognitionResult recognize(
        const CardPresentedEvent& event) override;

    /** @brief 返回 `recognize()` 被调用的次数。 */
    std::size_t callCount() const noexcept;

private:
    PlateRecognitionResult result_;
    std::size_t call_count_;
};

}  // namespace parking
