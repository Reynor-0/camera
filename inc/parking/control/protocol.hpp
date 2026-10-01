/**
 * @file protocol.hpp
 * @brief 定义 parkingctl 与 parkingd 之间的 PARKING/1 协议。
 */
#pragma once

#include "parking/domain/model.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace parking {

/** @brief 手动控制协议的当前线格式版本。 */
extern const char kManualControlProtocolVersion[];

/** @brief Unix seqpacket 单条控制消息允许的最大字节数。 */
const std::size_t kMaximumControlPacketBytes = 1024U;

/** @brief 表示 `parkingctl` 发给 `parkingd` 的一条刷卡请求。 */
struct ManualCardRequest {
    ManualCardRequest();

    std::string event_id;
    LaneDirection direction;
    std::string card_text;
};

/** @brief 表示服务端回复消息的类型。 */
enum class ControlResponseType {
    kAck,
    kReject,
    kFinal,
};

/** @brief 表示客户端解析后的 ACK、入口拒绝或最终业务结果。 */
struct ControlResponse {
    ControlResponse();

    ControlResponseType type;
    std::string event_id;
    std::string outcome;
    std::string reason;
    std::string session_id;
    std::string plate_number;
    std::int64_t parking_duration_seconds;
    std::int64_t fee_cent;
    bool replayed;
};

/**
 * @brief 序列化一条手动刷卡请求。
 * @param request event ID、方向和已规范化卡号。
 * @return 不含 NUL 的单个 `SOCK_SEQPACKET` payload。
 * @throws std::invalid_argument 字段为空、包含控制分隔符或 event ID 过长。
 */
std::string encodeManualCardRequest(const ManualCardRequest& request);

/**
 * @brief 解析并验证一条手动刷卡请求。
 * @param packet 完整 seqpacket payload。
 * @param request 非空输出指针；成功时写入请求。
 * @param error 非空输出指针；失败时写入稳定错误原因。
 * @return 协议版本、字段数量和基础字段合法时返回 true。
 */
bool parseManualCardRequest(const std::string& packet,
                            ManualCardRequest* request,
                            std::string* error) noexcept;

/**
 * @brief 生成已接受请求的 ACK 包。
 * @param event_id 已通过入口校验的 event ID。
 * @return 单个 ACK seqpacket payload。
 */
std::string encodeControlAck(const std::string& event_id);

/**
 * @brief 生成未进入业务状态机的 REJECT 包。
 * @param event_id 可识别的 event ID；完全无法解析时使用 `unknown`。
 * @param reason 拒绝原因，不得包含 tab 或换行。
 * @return 单个 REJECT seqpacket payload。
 */
std::string encodeControlReject(const std::string& event_id,
                                const std::string& reason);

/**
 * @brief 生成业务状态机的最终结果包。
 * @param result 已包含 event ID 的最终处理结果。
 * @return 单个 FINAL seqpacket payload。
 */
std::string encodeControlFinal(const AccessResult& result);

/**
 * @brief 解析 ACK、REJECT 或 FINAL 服务端消息。
 * @param packet 完整 seqpacket payload。
 * @param response 非空输出指针；成功时写入解析结果。
 * @param error 非空输出指针；失败时写入原因。
 * @return 消息版本、类型、字段数量和整数格式正确时返回 true。
 */
bool parseControlResponse(const std::string& packet,
                          ControlResponse* response,
                          std::string* error) noexcept;

}  // namespace parking
