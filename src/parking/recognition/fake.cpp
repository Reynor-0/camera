/*
 * 文件用途：实现可配置固定结果的模拟车牌识别器和识别结果校验。
 * 所属层次：recognition，不访问 Camera、Python 或模型。
 */
#include "parking/recognition/fake.hpp"

#include <stdexcept>

namespace parking {

PlateRecognitionResult::PlateRecognitionResult(
    bool success_value,
    const std::string& plate_value,
    std::uint32_t confidence_millionths_value)
    : success(success_value),
      plate_number(plate_value),
      confidence_millionths(confidence_millionths_value) {
    if (confidence_millionths > 1000000U) {
        throw std::invalid_argument(
            "plate confidence must be between 0 and 1000000");
    }
    if (success && plate_number.empty()) {
        throw std::invalid_argument(
            "successful plate recognition requires a plate number");
    }
    if (plate_number.size() > 64U ||
        plate_number.find('\t') != std::string::npos ||
        plate_number.find('\n') != std::string::npos ||
        plate_number.find('\r') != std::string::npos) {
        throw std::invalid_argument(
            "plate number is too long or contains a control delimiter");
    }
}

FakePlateRecognizer::FakePlateRecognizer()
    : result_(true, "TEST0001", 900000U), call_count_(0U) {}

void FakePlateRecognizer::setResult(const PlateRecognitionResult& result) {
    result_ = result;
}

PlateRecognitionResult FakePlateRecognizer::recognize(
    const CardPresentedEvent& event) {
    static_cast<void>(event);
    ++call_count_;
    return result_;
}

std::size_t FakePlateRecognizer::callCount() const noexcept {
    return call_count_;
}

}  // namespace parking
