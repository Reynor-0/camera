/*
 * 文件用途：验证 SQLite repository 的事务、重启恢复和持久幂等行为。
 */
#include "parking/domain/card.hpp"
#include "parking/domain/coordinator.hpp"
#include "parking/domain/fee.hpp"
#include "parking/domain/model.hpp"
#include "parking/recognition/fake.hpp"
#include "parking/storage/sqlite.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {

int failure_count = 0;

class TemporaryDatabase {
public:
    TemporaryDatabase() : path_() {
        char path_template[] = "/tmp/parking-sqlite-test-XXXXXX";
        const int descriptor = mkstemp(path_template);
        if (descriptor < 0) {
            throw std::runtime_error("mkstemp failed");
        }
        if (close(descriptor) != 0) {
            const int saved_errno = errno;
            unlink(path_template);
            errno = saved_errno;
            throw std::runtime_error("close temporary database failed");
        }
        if (unlink(path_template) != 0) {
            throw std::runtime_error("unlink temporary database failed");
        }
        path_ = path_template;
    }

    ~TemporaryDatabase() {
        unlink(path_.c_str());
        unlink((path_ + "-wal").c_str());
        unlink((path_ + "-shm").c_str());
    }

    TemporaryDatabase(const TemporaryDatabase&) = delete;
    TemporaryDatabase& operator=(const TemporaryDatabase&) = delete;

    const std::string& path() const noexcept { return path_; }

private:
    std::string path_;
};

void fail(const std::string& test_name, const std::string& detail) {
    ++failure_count;
    std::cerr << "[FAIL] " << test_name << ": " << detail << '\n';
}

void expectTrue(bool condition,
                const std::string& test_name,
                const std::string& detail) {
    if (!condition) {
        fail(test_name, detail);
    }
}

template <typename Actual, typename Expected>
void expectEqual(const Actual& actual,
                 const Expected& expected,
                 const std::string& test_name,
                 const std::string& detail) {
    if (!(actual == expected)) {
        fail(test_name, detail);
    }
}

parking::CardPresentedEvent makeEvent(
    const std::string& event_id,
    const std::string& card,
    parking::LaneDirection direction,
    std::int64_t utc_ms) {
    return parking::CardPresentedEvent(
        event_id, parking::CardId::fromText(card), direction,
        parking::CardEventSource::kManualSimulator, "local-cli-uid-1000",
        123456789U, utc_ms);
}

void testRestartPersistenceAndReplay() {
    const std::string name = "SQLite restart persistence and replay";
    TemporaryDatabase database;
    const parking::FeePolicy fee_policy(600, 3600, 500, 5000);
    const parking::CardPresentedEvent entry = makeEvent(
        "persist-entry-1", "04:A1:0B:7F", parking::LaneDirection::kEntry,
        1000000);
    std::string session_id;

    {
        parking::SQLiteParkingRepository repository(database.path());
        parking::FakePlateRecognizer recognizer;
        recognizer.setResult(
            parking::PlateRecognitionResult(true, "TEST1234", 920000U));
        parking::ParkingCoordinator coordinator(&repository, &recognizer,
                                                fee_policy);

        const parking::AccessResult result =
            coordinator.handleCardPresented(entry);
        expectEqual(result.outcome, parking::AccessOutcome::kEntryGranted,
                    name, "initial entry was not granted");
        expectTrue(!result.replayed, name,
                   "initial entry was incorrectly marked as replayed");
        expectEqual(repository.sessionCount(), static_cast<std::size_t>(1U),
                    name, "initial entry did not create one session");
        expectEqual(repository.activeSessionCount(),
                    static_cast<std::size_t>(1U), name,
                    "initial entry was not persisted as ACTIVE");
        session_id = result.session_id;
        repository.quickCheck();
    }

    {
        parking::SQLiteParkingRepository repository(database.path());
        parking::FakePlateRecognizer recognizer;
        recognizer.setResult(
            parking::PlateRecognitionResult(true, "WRONG999", 100000U));
        parking::ParkingCoordinator coordinator(&repository, &recognizer,
                                                fee_policy);

        expectEqual(repository.sessionCount(), static_cast<std::size_t>(1U),
                    name, "session history was lost after reopen");
        expectEqual(repository.activeSessionCount(),
                    static_cast<std::size_t>(1U), name,
                    "ACTIVE state was lost after reopen");

        const parking::AccessResult replay =
            coordinator.handleCardPresented(entry);
        expectTrue(replay.replayed, name,
                   "same event was not replayed after reopen");
        expectEqual(replay.session_id, session_id, name,
                    "replay returned a different session");
        expectEqual(replay.plate_number, std::string("TEST1234"), name,
                    "replay did not return the persisted plate");
        expectEqual(recognizer.callCount(), static_cast<std::size_t>(0U),
                    name, "replay invoked recognition after restart");

        const parking::CardPresentedEvent conflict = makeEvent(
            "persist-entry-1", "11223344", parking::LaneDirection::kEntry,
            1001000);
        const parking::AccessResult conflict_result =
            coordinator.handleCardPresented(conflict);
        expectEqual(conflict_result.reason,
                    parking::AccessReason::kEventIdConflict, name,
                    "changed payload reused an existing event ID");

        const parking::CardPresentedEvent duplicate = makeEvent(
            "persist-entry-duplicate", "04A10B7F",
            parking::LaneDirection::kEntry, 1002000);
        const parking::AccessResult duplicate_result =
            coordinator.handleCardPresented(duplicate);
        expectEqual(duplicate_result.reason,
                    parking::AccessReason::kDuplicateEntry, name,
                    "new event created a second ACTIVE session");
        expectEqual(recognizer.callCount(), static_cast<std::size_t>(0U),
                    name, "duplicate ACTIVE card invoked recognition");

        const parking::CardPresentedEvent exit = makeEvent(
            "persist-exit-1", "04A10B7F", parking::LaneDirection::kExit,
            4701000);
        const parking::AccessResult exit_result =
            coordinator.handleCardPresented(exit);
        expectEqual(exit_result.outcome, parking::AccessOutcome::kExitGranted,
                    name, "persisted ACTIVE session could not exit");
        expectEqual(exit_result.parking_duration_seconds,
                    static_cast<std::int64_t>(3701), name,
                    "persisted entry time produced the wrong duration");
        expectEqual(repository.activeSessionCount(),
                    static_cast<std::size_t>(0U), name,
                    "exit did not durably close the session");
        repository.quickCheck();
    }

    {
        parking::SQLiteParkingRepository repository(database.path());
        parking::FakePlateRecognizer recognizer;
        parking::ParkingCoordinator coordinator(&repository, &recognizer,
                                                fee_policy);
        const parking::CardPresentedEvent duplicate = makeEvent(
            "persist-entry-duplicate", "04A10B7F",
            parking::LaneDirection::kEntry, 1002000);
        const parking::AccessResult replayed_duplicate =
            coordinator.handleCardPresented(duplicate);
        expectTrue(replayed_duplicate.replayed, name,
                   "rejected result was not durable across reopen");
        expectEqual(replayed_duplicate.reason,
                    parking::AccessReason::kDuplicateEntry, name,
                    "rejected replay changed its reason");

        const parking::CardPresentedEvent exit = makeEvent(
            "persist-exit-1", "04A10B7F", parking::LaneDirection::kExit,
            4701000);
        const parking::AccessResult replayed_exit =
            coordinator.handleCardPresented(exit);
        expectTrue(replayed_exit.replayed, name,
                   "exit result was not durable across reopen");
        expectEqual(replayed_exit.outcome,
                    parking::AccessOutcome::kExitGranted, name,
                    "exit replay changed its outcome");
        expectEqual(repository.sessionCount(), static_cast<std::size_t>(1U),
                    name, "replays changed session history");
        expectEqual(repository.activeSessionCount(),
                    static_cast<std::size_t>(0U), name,
                    "closed session became ACTIVE after reopen");
        repository.quickCheck();
    }
}

void testExplicitRollback() {
    const std::string name = "SQLite explicit rollback";
    TemporaryDatabase database;
    parking::SQLiteParkingRepository repository(database.path());
    const parking::CardPresentedEvent entry = makeEvent(
        "rollback-entry", "A1B2C3D4", parking::LaneDirection::kEntry,
        9000000);
    const parking::ParkingSession session("session-rollback", entry,
                                          "ROLLBACK1", 800000U);

    repository.beginWriteTransaction();
    expectTrue(repository.createActive(session), name,
               "transaction setup insert failed");
    repository.rollbackTransaction();
    expectEqual(repository.sessionCount(), static_cast<std::size_t>(0U), name,
                "rolled-back session remained in the database");
    expectEqual(repository.activeSessionCount(), static_cast<std::size_t>(0U),
                name, "rolled-back ACTIVE state remained visible");
    repository.quickCheck();
}

}  // namespace

int main() {
    try {
        testRestartPersistenceAndReplay();
        testExplicitRollback();
    } catch (const std::exception& error) {
        fail("unexpected exception", error.what());
    }

    if (failure_count != 0) {
        std::cerr << failure_count << " SQLite repository assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "SQLite repository tests passed\n";
    return EXIT_SUCCESS;
}
