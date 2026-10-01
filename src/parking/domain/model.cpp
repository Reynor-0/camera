/*
 * 文件用途：实现停车事件、session 和业务结果的值类型及稳定字符串映射。
 * 所属层次：domain，不访问数据库、socket、Camera 或 DRM。
 */
#include "parking/domain/model.hpp"

namespace parking {

CardPresentedEvent::CardPresentedEvent(
    const std::string& event_id_value,
    const CardId& card_id,
    LaneDirection lane_direction,
    CardEventSource event_source,
    const std::string& source_name,
    std::uint64_t monotonic_ns,
    std::int64_t utc_ms)
    : event_id(event_id_value),
      card(card_id),
      direction(lane_direction),
      source(event_source),
      source_instance(source_name),
      receive_monotonic_ns(monotonic_ns),
      receive_utc_ms(utc_ms) {}

ParkingSession::ParkingSession(
    const std::string& session_id_value,
    const CardPresentedEvent& entry_event,
    const std::string& plate_value,
    std::uint32_t confidence_millionths_value)
    : session_id(session_id_value),
      entry_event_id(entry_event.event_id),
      card(entry_event.card),
      plate_number(plate_value),
      entry_confidence_millionths(confidence_millionths_value),
      entry_source(entry_event.source),
      entry_source_instance(entry_event.source_instance),
      entry_utc_ms(entry_event.receive_utc_ms),
      active(true),
      exit_event_id(),
      exit_source(entry_event.source),
      exit_source_instance(),
      exit_utc_ms(0),
      parking_duration_seconds(0),
      fee_cent(0) {}

AccessResult::AccessResult(AccessOutcome outcome_value,
                           AccessReason reason_value)
    : outcome(outcome_value),
      reason(reason_value),
      replayed(false),
      event_id(),
      session_id(),
      plate_number(),
      parking_duration_seconds(0),
      fee_cent(0) {}

const char* cardEventSourceName(CardEventSource source) noexcept {
    switch (source) {
        case CardEventSource::kSerialRfid:
            return "serial_rfid";
        case CardEventSource::kManualSimulator:
            return "manual_simulator";
    }
    return "unknown";
}

const char* accessOutcomeName(AccessOutcome outcome) noexcept {
    switch (outcome) {
        case AccessOutcome::kEntryGranted:
            return "ENTRY_GRANTED";
        case AccessOutcome::kExitGranted:
            return "EXIT_GRANTED";
        case AccessOutcome::kRejected:
            return "REJECTED";
    }
    return "UNKNOWN";
}

const char* accessReasonName(AccessReason reason) noexcept {
    switch (reason) {
        case AccessReason::kNone:
            return "NONE";
        case AccessReason::kDuplicateEntry:
            return "DUPLICATE_ENTRY";
        case AccessReason::kNoActiveSession:
            return "NO_ACTIVE_SESSION";
        case AccessReason::kRecognitionFailed:
            return "RECOGNITION_FAILED";
        case AccessReason::kInvalidEventId:
            return "INVALID_EVENT_ID";
        case AccessReason::kEventIdConflict:
            return "EVENT_ID_CONFLICT";
        case AccessReason::kInvalidEventTime:
            return "INVALID_EVENT_TIME";
        case AccessReason::kRepositoryConflict:
            return "REPOSITORY_CONFLICT";
    }
    return "UNKNOWN";
}

}  // namespace parking
