/**
 * @file memory.hpp
 * @brief 定义仅供测试使用的内存存储实现。
 */
#pragma once

#include "parking/storage/repository.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace parking {

/**
 * @brief 在内存 vector 中保存停车 session 的测试 repository。
 *
 * 数据随进程退出而消失，不能用于产品持久化。该实现用于验证业务状态机和 repository
 * 接口，不拥有文件或数据库连接，也不是线程安全的。
 */
class InMemoryParkingRepository : public ParkingRepository {
public:
    /** @brief 创建空 repository。 */
    InMemoryParkingRepository();

    void beginWriteTransaction() override;
    void commitTransaction() override;
    void rollbackTransaction() noexcept override;

    bool findActiveByCard(const CardId& card,
                          ParkingSession* session) const override;
    bool createActive(const ParkingSession& session) override;
    bool closeActive(const ParkingSession& session) override;
    bool findProcessedEvent(const std::string& event_id,
                            std::string* fingerprint,
                            AccessResult* result) const override;
    bool storeProcessedEvent(const CardPresentedEvent& event,
                             const std::string& fingerprint,
                             const AccessResult& result) override;

    /** @brief 返回全部历史 session 数量，包括已出场记录。 */
    std::size_t sessionCount() const noexcept override;

    /** @brief 返回当前 ACTIVE session 数量。 */
    std::size_t activeSessionCount() const noexcept override;

    /**
     * @brief 返回全部 session 的只读引用。
     * @return 引用有效期不超过 repository，后续写操作可能使其元素引用失效。
     */
    const std::vector<ParkingSession>& sessions() const noexcept;

private:
    struct StoredEvent {
        StoredEvent(const std::string& fingerprint_value,
                    const AccessResult& result_value);

        std::string fingerprint;
        AccessResult result;
    };

    std::vector<ParkingSession> sessions_;
    std::map<std::string, StoredEvent> events_;
    bool transaction_active_;
    std::vector<ParkingSession> transaction_sessions_snapshot_;
    std::map<std::string, StoredEvent> transaction_events_snapshot_;
};

}  // namespace parking
