/*
 * 文件用途：实现 RFID UID 的格式校验、规范化、比较和排序。
 * 所属层次：domain，不访问真实读卡器。
 */
#include "parking/domain/card.hpp"

#include <cctype>
#include <stdexcept>

namespace parking {

CardId::CardId(const std::string& normalized_value)
    : value_(normalized_value) {}

CardId CardId::fromText(const std::string& text) {
    std::string normalized;
    normalized.reserve(text.size());

    for (std::string::const_iterator it = text.begin(); it != text.end();
         ++it) {
        const unsigned char character =
            static_cast<unsigned char>(*it);
        if (*it == ':' || *it == '-' || std::isspace(character) != 0) {
            continue;
        }
        if (std::isxdigit(character) == 0) {
            throw std::invalid_argument(
                "RFID UID contains a non-hexadecimal character");
        }
        normalized.push_back(
            static_cast<char>(std::toupper(character)));
    }

    // ISO/IEC 14443 UID 常见长度为 4、7 或 10 字节。这里按完整字节数验证，
    // 避免奇数个十六进制字符被静默补零并改变卡号身份。
    if (normalized.size() != 8U && normalized.size() != 14U &&
        normalized.size() != 20U) {
        throw std::invalid_argument(
            "RFID UID must contain exactly 4, 7, or 10 bytes");
    }

    return CardId(normalized);
}

const std::string& CardId::value() const noexcept {
    return value_;
}

bool CardId::operator==(const CardId& other) const noexcept {
    return value_ == other.value_;
}

bool CardId::operator!=(const CardId& other) const noexcept {
    return !(*this == other);
}

bool CardId::operator<(const CardId& other) const noexcept {
    return value_ < other.value_;
}

}  // namespace parking
