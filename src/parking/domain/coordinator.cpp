/*
 * 文件用途：编排入场、出场、识别、计费、事务和 event 幂等处理。
 * 所属层次：domain，只通过 repository/recognizer 接口访问外部能力。
 */
#include "parking/domain/coordinator.hpp"

#include <sstream>
#include <stdexcept>

namespace parking {

namespace {

class TransactionGuard {
public:
    explicit TransactionGuard(ParkingRepository* repository)
        : repository_(repository), committed_(false) {
        repository_->beginWriteTransaction();
    }

    ~TransactionGuard() {
        if (!committed_) {
            repository_->rollbackTransaction();
        }
    }

    void commit() {
        repository_->commitTransaction();
        committed_ = true;
    }

private:
    ParkingRepository* repository_;
    bool committed_;
};

}  // namespace

ParkingCoordinator::ParkingCoordinator(ParkingRepository* repository,
                                       PlateRecognizer* recognizer,
                                       const FeePolicy& fee_policy)
    : repository_(repository),
      recognizer_(recognizer),
      fee_policy_(fee_policy) {
    if (repository_ == nullptr || recognizer_ == nullptr) {
        throw std::invalid_argument(
            "ParkingCoordinator dependencies cannot be null");
    }
}

AccessResult ParkingCoordinator::handleCardPresented(
    const CardPresentedEvent& event) {
    if (event.event_id.empty()) {
        AccessResult invalid(AccessOutcome::kRejected,
                             AccessReason::kInvalidEventId);
        invalid.event_id = event.event_id;
        return invalid;
    }

    std::string stored_fingerprint;
    AccessResult stored_result(AccessOutcome::kRejected, AccessReason::kNone);
    if (repository_->findProcessedEvent(event.event_id, &stored_fingerprint,
                                        &stored_result)) {
        if (stored_fingerprint != eventFingerprint(event)) {
            AccessResult conflict(AccessOutcome::kRejected,
                                  AccessReason::kEventIdConflict);
            conflict.event_id = event.event_id;
            return conflict;
        }
        AccessResult replayed = stored_result;
        replayed.replayed = true;
        return replayed;
    }

    if (event.direction == LaneDirection::kEntry) {
        return handleEntry(event);
    }
    return handleExit(event);
}

AccessResult ParkingCoordinator::handleEntry(
    const CardPresentedEvent& event) {
    ParkingSession active_session("lookup-placeholder", event, "PENDING", 0U);
    if (repository_->findActiveByCard(event.card, &active_session)) {
        AccessResult duplicate(AccessOutcome::kRejected,
                               AccessReason::kDuplicateEntry);
        duplicate.session_id = active_session.session_id;
        duplicate.plate_number = active_session.plate_number;
        return storeStandaloneResult(event, duplicate);
    }

    const PlateRecognitionResult recognition = recognizer_->recognize(event);
    if (!recognition.success || recognition.plate_number.empty()) {
        const AccessResult failed(AccessOutcome::kRejected,
                                  AccessReason::kRecognitionFailed);
        return storeStandaloneResult(event, failed);
    }

    // 识别等外部耗时工作已经结束后才开始写事务。事务内再次检查 ACTIVE 不变量，
    // 为以后事件循环并发来源接入保留正确的提交边界。
    TransactionGuard transaction(repository_);
    if (repository_->findActiveByCard(event.card, &active_session)) {
        AccessResult duplicate(AccessOutcome::kRejected,
                               AccessReason::kDuplicateEntry);
        duplicate.session_id = active_session.session_id;
        duplicate.plate_number = active_session.plate_number;
        const AccessResult stored =
            storeResultInCurrentTransaction(event, duplicate);
        transaction.commit();
        return stored;
    }

    const ParkingSession new_session(sessionIdForEvent(event), event,
                                     recognition.plate_number,
                                     recognition.confidence_millionths);
    AccessResult result(AccessOutcome::kEntryGranted, AccessReason::kNone);
    if (!repository_->createActive(new_session)) {
        result.outcome = AccessOutcome::kRejected;
        result.reason = AccessReason::kRepositoryConflict;
    } else {
        result.session_id = new_session.session_id;
        result.plate_number = new_session.plate_number;
    }
    const AccessResult stored =
        storeResultInCurrentTransaction(event, result);
    transaction.commit();
    return stored;
}

AccessResult ParkingCoordinator::handleExit(
    const CardPresentedEvent& event) {
    TransactionGuard transaction(repository_);
    ParkingSession session("lookup-placeholder", event, "PENDING", 0U);
    AccessResult result(AccessOutcome::kRejected, AccessReason::kNone);
    if (!repository_->findActiveByCard(event.card, &session)) {
        result.reason = AccessReason::kNoActiveSession;
        const AccessResult stored =
            storeResultInCurrentTransaction(event, result);
        transaction.commit();
        return stored;
    }
    if (event.receive_utc_ms < session.entry_utc_ms) {
        result.reason = AccessReason::kInvalidEventTime;
        result.session_id = session.session_id;
        result.plate_number = session.plate_number;
        const AccessResult stored =
            storeResultInCurrentTransaction(event, result);
        transaction.commit();
        return stored;
    }

    const std::int64_t duration_seconds =
        (event.receive_utc_ms - session.entry_utc_ms) / 1000;
    const std::int64_t fee_cent = fee_policy_.calculate(duration_seconds);

    session.active = false;
    session.exit_event_id = event.event_id;
    session.exit_source = event.source;
    session.exit_source_instance = event.source_instance;
    session.exit_utc_ms = event.receive_utc_ms;
    session.parking_duration_seconds = duration_seconds;
    session.fee_cent = fee_cent;

    if (!repository_->closeActive(session)) {
        result.reason = AccessReason::kRepositoryConflict;
    } else {
        result.outcome = AccessOutcome::kExitGranted;
        result.reason = AccessReason::kNone;
        result.session_id = session.session_id;
        result.plate_number = session.plate_number;
        result.parking_duration_seconds = duration_seconds;
        result.fee_cent = fee_cent;
    }
    const AccessResult stored =
        storeResultInCurrentTransaction(event, result);
    transaction.commit();
    return stored;
}

std::string ParkingCoordinator::sessionIdForEvent(
    const CardPresentedEvent& event) const {
    return std::string("session-") + event.event_id;
}

std::string ParkingCoordinator::eventFingerprint(
    const CardPresentedEvent& event) const {
    std::ostringstream stream;
    stream << event.card.value() << '|'
           << (event.direction == LaneDirection::kEntry ? "entry" : "exit")
           << '|' << cardEventSourceName(event.source) << '|'
           << event.source_instance;
    return stream.str();
}

AccessResult ParkingCoordinator::storeStandaloneResult(
    const CardPresentedEvent& event,
    const AccessResult& result) {
    TransactionGuard transaction(repository_);
    const AccessResult stored =
        storeResultInCurrentTransaction(event, result);
    transaction.commit();
    return stored;
}

AccessResult ParkingCoordinator::storeResultInCurrentTransaction(
    const CardPresentedEvent& event,
    const AccessResult& result) {
    AccessResult stored = result;
    stored.event_id = event.event_id;
    stored.replayed = false;
    if (!repository_->storeProcessedEvent(
            event, eventFingerprint(event), stored)) {
        throw std::runtime_error(
            "processed event ID already exists during transaction");
    }
    return stored;
}

/*
 * The transaction-aware implementation above intentionally keeps recognition
 * outside BEGIN IMMEDIATE. Do not move external Camera/LPR IPC into the write
 * transaction when the fake recognizer is replaced.
 */

}  // namespace parking
