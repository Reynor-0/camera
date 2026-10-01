/**
 * @file fee.hpp
 * @brief 定义整数秒、整数分的停车计费策略。
 */
#pragma once

#include <cstdint>

namespace parking {

/**
 * @brief 使用整数秒和整数分计算演示停车费用。
 *
 * 超过免费时长后按计费单元向上取整，并限制在单次费用上限。对象为不可变值类型，
 * 本身不需要线程同步。
 */
class FeePolicy {
public:
    /**
     * @brief 创建一套计费参数。
     * @param free_seconds 免费停车秒数，必须非负。
     * @param billing_unit_seconds 每个计费单元秒数，必须大于零。
     * @param unit_fee_cent 每个计费单元的整数分价格，必须非负。
     * @param maximum_fee_cent 单次费用上限，必须非负。
     * @throws std::invalid_argument 参数越界。
     */
    FeePolicy(std::int64_t free_seconds,
              std::int64_t billing_unit_seconds,
              std::int64_t unit_fee_cent,
              std::int64_t maximum_fee_cent);

    /**
     * @brief 计算给定停车时长的费用。
     * @param duration_seconds 停车时长秒数，必须非负。
     * @return 经过免费时长、向上取整和上限约束后的整数分费用。
     * @throws std::invalid_argument 时长为负数。
     */
    std::int64_t calculate(std::int64_t duration_seconds) const;

private:
    std::int64_t free_seconds_;
    std::int64_t billing_unit_seconds_;
    std::int64_t unit_fee_cent_;
    std::int64_t maximum_fee_cent_;
};

}  // namespace parking
