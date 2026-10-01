/*
 * 文件用途：实现测试专用的内存 repository 和事务快照回滚。
 * 所属层次：storage，进程退出后数据消失，不用于板端生产持久化。
 */
#include "parking/storage/memory.hpp"

#include <stdexcept>

namespace parking {

InMemoryParkingRepository::StoredEvent::StoredEvent(
    const std::string& fingerprint_value,
    const AccessResult& result_value)
    : fingerprint(fingerprint_value), result(result_value) {}

InMemoryParkingRepository::InMemoryParkingRepository()
    : sessions_(),
      events_(),
      transaction_active_(false),
      transaction_sessions_snapshot_(),
      transaction_events_snapshot_() {}

void InMemoryParkingRepository::beginWriteTransaction() {
    if (transaction_active_) {
        throw std::runtime_error("in-memory transaction is already active");
    }
    transaction_sessions_snapshot_ = sessions_;
    transaction_events_snapshot_ = events_;
    transaction_active_ = true;
}

void InMemoryParkingRepository::commitTransaction() {
    if (!transaction_active_) {
        throw std::runtime_error("no in-memory transaction is active");
    }
    transaction_sessions_snapshot_.clear();
    transaction_events_snapshot_.clear();
    transaction_active_ = false;
}

void InMemoryParkingRepository::rollbackTransaction() noexcept {
    if (!transaction_active_) {
        return;
    }
    sessions_.swap(transaction_sessions_snapshot_);
    events_.swap(transaction_events_snapshot_);
    transaction_sessions_snapshot_.clear();
    transaction_events_snapshot_.clear();
    transaction_active_ = false;
}

bool InMemoryParkingRepository::findActiveByCard(
    const CardId& card,
    ParkingSession* session) const {
    if (session == nullptr) {
        throw std::invalid_argument(
            "findActiveByCard requires a non-null output pointer");
    }

    for (std::vector<ParkingSession>::const_iterator it = sessions_.begin();
         it != sessions_.end(); ++it) {
        if (it->active && it->card == card) {
            *session = *it;
            return true;
        }
    }
    return false;
}

bool InMemoryParkingRepository::createActive(
    const ParkingSession& session) {
    if (!session.active) {
        return false;
    }

    for (std::vector<ParkingSession>::const_iterator it = sessions_.begin();
         it != sessions_.end(); ++it) {
        if (it->session_id == session.session_id ||
            (it->active && it->card == session.card)) {
            return false;
        }
    }
    sessions_.push_back(session);
    return true;
}

bool InMemoryParkingRepository::closeActive(
    const ParkingSession& session) {
    if (session.active) {
        return false;
    }

    for (std::vector<ParkingSession>::iterator it = sessions_.begin();
         it != sessions_.end(); ++it) {
        if (it->active && it->session_id == session.session_id &&
            it->card == session.card) {
            *it = session;
            return true;
        }
    }
    return false;
}

bool InMemoryParkingRepository::findProcessedEvent(
    const std::string& event_id,
    std::string* fingerprint,
    AccessResult* result) const {
    if (fingerprint == nullptr || result == nullptr) {
        throw std::invalid_argument(
            "findProcessedEvent requires non-null output pointers");
    }
    const std::map<std::string, StoredEvent>::const_iterator found =
        events_.find(event_id);
    if (found == events_.end()) {
        return false;
    }
    *fingerprint = found->second.fingerprint;
    *result = found->second.result;
    return true;
}

bool InMemoryParkingRepository::storeProcessedEvent(
    const CardPresentedEvent& event,
    const std::string& fingerprint,
    const AccessResult& result) {
    if (!transaction_active_) {
        throw std::runtime_error(
            "storeProcessedEvent requires an active transaction");
    }
    if (result.event_id != event.event_id || fingerprint.empty()) {
        throw std::invalid_argument("processed event metadata is inconsistent");
    }
    return events_.insert(std::make_pair(
        event.event_id, StoredEvent(fingerprint, result))).second;
}

std::size_t InMemoryParkingRepository::sessionCount() const noexcept {
    return sessions_.size();
}

std::size_t InMemoryParkingRepository::activeSessionCount() const noexcept {
    std::size_t count = 0U;
    for (std::vector<ParkingSession>::const_iterator it = sessions_.begin();
         it != sessions_.end(); ++it) {
        if (it->active) {
            ++count;
        }
    }
    return count;
}

const std::vector<ParkingSession>&
InMemoryParkingRepository::sessions() const noexcept {
    return sessions_;
}

}  // namespace parking
