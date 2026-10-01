/**
 * @file coordinator.hpp
 * @brief 定义入场、出场和幂等处理的业务协调器。
 */
#pragma once

#include "parking/domain/fee.hpp"
#include "parking/domain/model.hpp"
#include "parking/recognition/recognizer.hpp"
#include "parking/storage/repository.hpp"

#include <cstdint>
#include <string>

namespace parking {

/**
 * @brief 执行单车道停车入场和出场状态机。
 *
 * Coordinator 不拥有注入的 repository 和 recognizer，它们必须比本对象存活更久。当前
 * 实现同步处理事件且不是线程安全的；`parkingd` 在单 writer 事件循环中调用它。已处理
 * event ID 的最终结果由 repository 保存，因此 SQLite 实现可以跨进程重启恢复幂等状态。
 */
class ParkingCoordinator {
public:
    /**
     * @brief 装配业务状态机依赖。
     * @param repository 非空 session repository，由调用方拥有。
     * @param recognizer 非空车牌识别 adapter，由调用方拥有。
     * @param fee_policy 不可变计费策略副本。
     * @throws std::invalid_argument 任一依赖指针为空。
     */
    ParkingCoordinator(ParkingRepository* repository,
                       PlateRecognizer* recognizer,
                       const FeePolicy& fee_policy);

    /**
     * @brief 同步处理一次真实或模拟刷卡事件。
     *
     * Entry 会验证重复入场、调用识别器并创建 ACTIVE session；Exit 会查询 ACTIVE
     * session、计算费用并关闭记录。同一 event ID 重放时返回缓存结果且不重复修改存储。
     *
     * @param event 已规范化的统一卡事件。
     * @return 最终业务结果；`replayed=true` 表示命中 repository 中的幂等记录。
     */
    AccessResult handleCardPresented(const CardPresentedEvent& event);

private:
    AccessResult handleEntry(const CardPresentedEvent& event);
    AccessResult handleExit(const CardPresentedEvent& event);
    std::string sessionIdForEvent(const CardPresentedEvent& event) const;
    std::string eventFingerprint(const CardPresentedEvent& event) const;
    AccessResult storeStandaloneResult(const CardPresentedEvent& event,
                                       const AccessResult& result);
    AccessResult storeResultInCurrentTransaction(
        const CardPresentedEvent& event,
        const AccessResult& result);

    ParkingRepository* repository_;
    PlateRecognizer* recognizer_;
    FeePolicy fee_policy_;
};

}  // namespace parking
