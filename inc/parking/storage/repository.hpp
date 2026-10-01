/**
 * @file repository.hpp
 * @brief 定义停车 session 和 event journal 的存储接口。
 */
#pragma once

#include "parking/domain/model.hpp"

#include <cstddef>
#include <string>

namespace parking {

/**
 * @brief 定义停车 session 的存储边界。
 *
 * SQLite adapter 与测试用内存 adapter 都必须保持同一不变量：一个卡号最多只有一个
 * ACTIVE session。接口默认不是线程安全的，由 `parkingd` 单 writer 调用。
 */
class ParkingRepository {
public:
    virtual ~ParkingRepository() {}

    /**
     * @brief 开始一个排他的业务写事务。
     * @throws std::runtime_error 已有事务或后端无法开始事务。
     */
    virtual void beginWriteTransaction() = 0;

    /**
     * @brief 原子提交当前 session 和 event journal 修改。
     * @throws std::runtime_error 没有事务或提交失败。
     */
    virtual void commitTransaction() = 0;

    /** @brief 回滚当前事务；没有活动事务时不执行操作。 */
    virtual void rollbackTransaction() noexcept = 0;

    /**
     * @brief 查找指定卡号的 ACTIVE session。
     * @param card 待查询的规范化卡号。
     * @param session 非空输出指针；找到时写入 session 副本。
     * @return 找到 ACTIVE session 时返回 true，否则返回 false。
     */
    virtual bool findActiveByCard(const CardId& card,
                                  ParkingSession* session) const = 0;

    /**
     * @brief 插入一条新的 ACTIVE session。
     * @param session 待插入的 session，必须处于 active 状态。
     * @return 插入成功返回 true；卡号已有 ACTIVE session 或 ID 冲突时返回 false。
     */
    virtual bool createActive(const ParkingSession& session) = 0;

    /**
     * @brief 用已经关闭的副本替换对应 ACTIVE session。
     * @param session `active=false` 且包含出场结果的 session。
     * @return 成功关闭返回 true；原 ACTIVE session 不存在时返回 false。
     */
    virtual bool closeActive(const ParkingSession& session) = 0;

    /**
     * @brief 查询已经完成的 event ID 及其原始最终结果。
     * @param event_id 全局幂等事件 ID。
     * @param fingerprint 非空输出指针；找到时写入输入 payload 指纹。
     * @param result 非空输出指针；找到时写入原始最终结果。
     * @return 找到事件时返回 true，否则返回 false。
     */
    virtual bool findProcessedEvent(const std::string& event_id,
                                    std::string* fingerprint,
                                    AccessResult* result) const = 0;

    /**
     * @brief 在当前写事务中保存一条最终事件结果。
     * @param event 产生最终结果的原始统一卡事件。
     * @param fingerprint 不含接收时间的稳定输入 payload 指纹。
     * @param result 最终业务结果，event ID 必须与 event 相同。
     * @return 插入成功返回 true；event ID 已存在时返回 false。
     */
    virtual bool storeProcessedEvent(const CardPresentedEvent& event,
                                     const std::string& fingerprint,
                                     const AccessResult& result) = 0;

    /** @brief 返回全部历史 session 数量。 */
    virtual std::size_t sessionCount() const = 0;

    /** @brief 返回当前 ACTIVE session 数量。 */
    virtual std::size_t activeSessionCount() const = 0;
};

}  // namespace parking
