/**
 * @file card.hpp
 * @brief 定义 RFID 卡号值对象及规范化接口。
 */
#pragma once

#include <string>

namespace parking {

/**
 * @brief 表示已经规范化的 RFID 卡 UID。
 *
 * 卡号内部保存为不带分隔符的大写十六进制字符串。对象创建后始终有效并保留前导零。
 * 该值类型不拥有硬件资源，可以安全复制；对象本身不提供线程同步。
 */
class CardId {
public:
    /**
     * @brief 解析常见的人类可读 RFID UID。
     *
     * 输入可以包含冒号、短横线和 ASCII 空白，移除分隔符后必须是 4、7 或 10 字节的
     * 十六进制 UID。
     *
     * @param text 待解析的卡号文本。
     * @return 规范化后的卡号。
     * @throws std::invalid_argument 输入为空、包含非法字符或 UID 长度不受支持。
     */
    static CardId fromText(const std::string& text);

    /** @brief 返回不带分隔符的大写十六进制 UID。 */
    const std::string& value() const noexcept;

    /** @brief 判断两个规范化卡号是否相同。 */
    bool operator==(const CardId& other) const noexcept;

    /** @brief 判断两个规范化卡号是否不同。 */
    bool operator!=(const CardId& other) const noexcept;

    /** @brief 为有序容器提供稳定排序。 */
    bool operator<(const CardId& other) const noexcept;

private:
    explicit CardId(const std::string& normalized_value);

    std::string value_;
};

}  // namespace parking
