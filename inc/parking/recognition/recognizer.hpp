/**
 * @file recognizer.hpp
 * @brief 定义车牌识别结果和可替换的识别接口。
 */
#pragma once

#include "parking/domain/model.hpp"

#include <cstdint>
#include <string>

namespace parking {

/** @brief 表示一次同步车牌识别的结果。 */
struct PlateRecognitionResult {
    /**
     * @brief 创建识别结果。
     * @param success_value 是否取得可接受车牌。
     * @param plate_value UTF-8 车牌；失败时为空。
     * @param confidence_millionths_value 置信度，范围 0~1000000。
     */
    PlateRecognitionResult(bool success_value,
                           const std::string& plate_value,
                           std::uint32_t confidence_millionths_value);

    bool success;
    std::string plate_number;
    std::uint32_t confidence_millionths;
};

/**
 * @brief 定义停车业务获取车牌结果的抽象边界。
 *
 * 第一阶段实现固定结果 adapter。后续 Camera/HyperLPR IPC adapter 可以替换实现而不改变
 * `ParkingCoordinator`。接口和实现默认不是线程安全的。
 */
class PlateRecognizer {
public:
    virtual ~PlateRecognizer() {}

    /**
     * @brief 为当前入场事件取得车牌结果。
     * @param event 已通过基本验证的入场卡事件。
     * @return 当前请求的识别结果。
     */
    virtual PlateRecognitionResult recognize(
        const CardPresentedEvent& event) = 0;
};

}  // namespace parking
