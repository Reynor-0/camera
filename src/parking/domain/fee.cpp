/*
 * 文件用途：实现停车时长到整数分费用的确定性计算。
 * 所属层次：domain，不读取系统时间或数据库。
 */
#include "parking/domain/fee.hpp"

#include <stdexcept>

namespace parking {

FeePolicy::FeePolicy(std::int64_t free_seconds,
                     std::int64_t billing_unit_seconds,
                     std::int64_t unit_fee_cent,
                     std::int64_t maximum_fee_cent)
    : free_seconds_(free_seconds),
      billing_unit_seconds_(billing_unit_seconds),
      unit_fee_cent_(unit_fee_cent),
      maximum_fee_cent_(maximum_fee_cent) {
    if (free_seconds_ < 0 || billing_unit_seconds_ <= 0 ||
        unit_fee_cent_ < 0 || maximum_fee_cent_ < 0) {
        throw std::invalid_argument("invalid parking fee policy");
    }
}

std::int64_t FeePolicy::calculate(std::int64_t duration_seconds) const {
    if (duration_seconds < 0) {
        throw std::invalid_argument("parking duration cannot be negative");
    }
    if (duration_seconds <= free_seconds_ || unit_fee_cent_ == 0 ||
        maximum_fee_cent_ == 0) {
        return 0;
    }

    const std::int64_t billable_seconds = duration_seconds - free_seconds_;
    // 使用 1 + (n - 1) / unit 完成向上取整，避免 n + unit - 1 溢出。
    const std::int64_t units =
        1 + (billable_seconds - 1) / billing_unit_seconds_;
    if (units > maximum_fee_cent_ / unit_fee_cent_) {
        return maximum_fee_cent_;
    }

    const std::int64_t calculated_fee = units * unit_fee_cent_;
    return calculated_fee > maximum_fee_cent_ ? maximum_fee_cent_
                                               : calculated_fee;
}

}  // namespace parking
