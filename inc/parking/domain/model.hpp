/**
 * @file model.hpp
 * @brief 定义停车业务事件、session、结果和稳定枚举。
 */
#pragma once

#include "parking/domain/card.hpp"

#include <cstdint>
#include <string>

namespace parking {

/** @brief 表示刷卡请求对应入口车道还是出口车道。 */
enum class LaneDirection {
    kEntry,
    kExit,
};

/** @brief 标识卡事件来自真实串口读卡器还是本地手动模拟器。 */
enum class CardEventSource {
    kSerialRfid,
    kManualSimulator,
};

/** @brief 表示一次访问请求的最终业务结果。 */
enum class AccessOutcome {
    kEntryGranted,
    kExitGranted,
    kRejected,
};

/** @brief 表示一次访问请求被拒绝或成功完成的稳定原因码。 */
enum class AccessReason {
    kNone,
    kDuplicateEntry,
    kNoActiveSession,
    kRecognitionFailed,
    kInvalidEventId,
    kEventIdConflict,
    kInvalidEventTime,
    kRepositoryConflict,
};

/**
 * @brief 表示由任意卡事件 adapter 产生的统一业务输入。
 *
 * 时间由事件来源在接收边界采集。UTC 毫秒用于业务记录，monotonic 纳秒用于进程内耗时
 * 和超时判断。结构体不拥有 fd，也不携带允许放行等策略字段。
 */
struct CardPresentedEvent {
    /**
     * @brief 构造一条完整的刷卡事件。
     * @param event_id 本次物理或模拟触发的唯一 ID，用于幂等。
     * @param card_id 已规范化的卡号。
     * @param lane_direction 本次请求的入场或出场方向。
     * @param event_source 真实 RFID 或手动模拟来源。
     * @param source_name 来源实例名，例如 entry-reader-0 或 local-cli。
     * @param monotonic_ns 接收事件时的单调时钟纳秒值。
     * @param utc_ms 接收事件时的 UTC Unix epoch 毫秒值。
     */
    CardPresentedEvent(const std::string& event_id,
                       const CardId& card_id,
                       LaneDirection lane_direction,
                       CardEventSource event_source,
                       const std::string& source_name,
                       std::uint64_t monotonic_ns,
                       std::int64_t utc_ms);

    std::string event_id;
    CardId card;
    LaneDirection direction;
    CardEventSource source;
    std::string source_instance;
    std::uint64_t receive_monotonic_ns;
    std::int64_t receive_utc_ms;
};

/**
 * @brief 表示一条停车 session 的业务状态。
 *
 * `parkingd` 通过 SQLite adapter 持久化这些字段；内存 adapter 仅用于单元测试。
 */
struct ParkingSession {
    /**
     * @brief 创建一条尚未出场的 ACTIVE session。
     * @param session_id_value 由协调器生成的唯一 session ID。
     * @param entry_event 建立该 session 的入场事件。
     * @param plate_value 车牌识别结果。
     * @param confidence_millionths_value 车牌置信度，范围 0~1000000。
     */
    ParkingSession(const std::string& session_id_value,
                   const CardPresentedEvent& entry_event,
                   const std::string& plate_value,
                   std::uint32_t confidence_millionths_value);

    std::string session_id;
    std::string entry_event_id;
    CardId card;
    std::string plate_number;
    std::uint32_t entry_confidence_millionths;
    CardEventSource entry_source;
    std::string entry_source_instance;
    std::int64_t entry_utc_ms;
    bool active;
    std::string exit_event_id;
    CardEventSource exit_source;
    std::string exit_source_instance;
    std::int64_t exit_utc_ms;
    std::int64_t parking_duration_seconds;
    std::int64_t fee_cent;
};

/** @brief 保存同步处理一条卡事件后返回给调用方的结果。 */
struct AccessResult {
    /** @brief 创建一条具有明确结果和原因的返回值。 */
    AccessResult(AccessOutcome outcome_value, AccessReason reason_value);

    AccessOutcome outcome;
    AccessReason reason;
    bool replayed;
    std::string event_id;
    std::string session_id;
    std::string plate_number;
    std::int64_t parking_duration_seconds;
    std::int64_t fee_cent;
};

/**
 * @brief 返回事件来源的稳定配置/数据库名称。
 * @param source 待转换的事件来源。
 * @return `serial_rfid` 或 `manual_simulator`。
 */
const char* cardEventSourceName(CardEventSource source) noexcept;

/**
 * @brief 返回访问结果的稳定日志名称。
 * @param outcome 待转换的访问结果。
 * @return 可用于日志和测试输出的静态字符串。
 */
const char* accessOutcomeName(AccessOutcome outcome) noexcept;

/**
 * @brief 返回访问原因的稳定日志名称。
 * @param reason 待转换的访问原因。
 * @return 可用于日志和测试输出的静态字符串。
 */
const char* accessReasonName(AccessReason reason) noexcept;

}  // namespace parking
