/*
 * 文件用途：验证停车领域模型、状态机、计费和内存 repository。
 */
#include "parking/domain/card.hpp"
#include "parking/domain/coordinator.hpp"
#include "parking/domain/fee.hpp"
#include "parking/domain/model.hpp"
#include "parking/recognition/fake.hpp"
#include "parking/storage/memory.hpp"

#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

int failure_count = 0;

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
    parking::CardEventSource source,
    const std::string& source_instance,
    std::int64_t utc_ms) {
    return parking::CardPresentedEvent(
        event_id, parking::CardId::fromText(card), direction, source,
        source_instance, 1000000U, utc_ms);
}

void testCardIdNormalization() {
    const std::string name = "CardId normalization";
    const parking::CardId card = parking::CardId::fromText("04:a1-0b 7f");
    expectEqual(card.value(), std::string("04A10B7F"), name,
                "separators, case, or leading zero were not normalized");

    bool invalid_character_rejected = false;
    try {
        static_cast<void>(parking::CardId::fromText("04A10B7Z"));
    } catch (const std::invalid_argument&) {
        invalid_character_rejected = true;
    }
    expectTrue(invalid_character_rejected, name,
               "non-hexadecimal UID was accepted");

    bool invalid_length_rejected = false;
    try {
        static_cast<void>(parking::CardId::fromText("123456"));
    } catch (const std::invalid_argument&) {
        invalid_length_rejected = true;
    }
    expectTrue(invalid_length_rejected, name,
               "unsupported UID length was accepted");
}

void testManualEntryAndIdempotentReplay() {
    const std::string name = "manual entry and replay";
    parking::InMemoryParkingRepository repository;
    parking::FakePlateRecognizer recognizer;
    recognizer.setResult(
        parking::PlateRecognitionResult(true, "TEST1234", 920000U));
    const parking::FeePolicy fee_policy(600, 3600, 500, 5000);
    parking::ParkingCoordinator coordinator(&repository, &recognizer,
                                            fee_policy);

    const parking::CardPresentedEvent event = makeEvent(
        "manual-entry-1", "04A10B7F", parking::LaneDirection::kEntry,
        parking::CardEventSource::kManualSimulator, "local-cli", 1000000);
    const parking::AccessResult first = coordinator.handleCardPresented(event);

    expectEqual(first.outcome, parking::AccessOutcome::kEntryGranted, name,
                "new card was not granted entry");
    expectEqual(first.reason, parking::AccessReason::kNone, name,
                "successful entry returned a failure reason");
    expectTrue(!first.replayed, name, "first event was marked as replayed");
    expectEqual(repository.sessionCount(), static_cast<std::size_t>(1U), name,
                "entry did not create exactly one session");
    expectEqual(repository.activeSessionCount(),
                static_cast<std::size_t>(1U), name,
                "entry session was not ACTIVE");
    expectEqual(recognizer.callCount(), static_cast<std::size_t>(1U), name,
                "entry did not invoke fake recognizer exactly once");
    expectEqual(repository.sessions().front().entry_source,
                parking::CardEventSource::kManualSimulator, name,
                "manual source was not retained in the session");

    const parking::AccessResult replayed =
        coordinator.handleCardPresented(event);
    expectTrue(replayed.replayed, name,
               "same event ID was not reported as a replay");
    expectEqual(replayed.session_id, first.session_id, name,
                "replayed event changed the session ID");
    expectEqual(repository.sessionCount(), static_cast<std::size_t>(1U), name,
                "replayed event created a second session");
    expectEqual(recognizer.callCount(), static_cast<std::size_t>(1U), name,
                "replayed event invoked recognition again");
}

void testDuplicateEntryAcrossSources() {
    const std::string name = "duplicate entry across sources";
    parking::InMemoryParkingRepository repository;
    parking::FakePlateRecognizer recognizer;
    const parking::FeePolicy fee_policy(600, 3600, 500, 5000);
    parking::ParkingCoordinator coordinator(&repository, &recognizer,
                                            fee_policy);

    const parking::CardPresentedEvent manual_entry = makeEvent(
        "manual-entry-2", "11223344", parking::LaneDirection::kEntry,
        parking::CardEventSource::kManualSimulator, "local-cli", 2000000);
    const parking::CardPresentedEvent serial_entry = makeEvent(
        "serial-entry-1", "11:22:33:44", parking::LaneDirection::kEntry,
        parking::CardEventSource::kSerialRfid, "entry-reader-0", 2001000);

    static_cast<void>(coordinator.handleCardPresented(manual_entry));
    const parking::AccessResult duplicate =
        coordinator.handleCardPresented(serial_entry);
    expectEqual(duplicate.outcome, parking::AccessOutcome::kRejected, name,
                "second source created a duplicate entry");
    expectEqual(duplicate.reason, parking::AccessReason::kDuplicateEntry, name,
                "duplicate entry returned the wrong reason");
    expectEqual(repository.sessionCount(), static_cast<std::size_t>(1U), name,
                "two sources created two sessions for one active card");
    expectEqual(recognizer.callCount(), static_cast<std::size_t>(1U), name,
                "duplicate entry unnecessarily invoked recognition");
}

void testEventIdPayloadConflict() {
    const std::string name = "event ID payload conflict";
    parking::InMemoryParkingRepository repository;
    parking::FakePlateRecognizer recognizer;
    const parking::FeePolicy fee_policy(600, 3600, 500, 5000);
    parking::ParkingCoordinator coordinator(&repository, &recognizer,
                                            fee_policy);

    const parking::CardPresentedEvent first = makeEvent(
        "reused-event", "10111213", parking::LaneDirection::kEntry,
        parking::CardEventSource::kManualSimulator, "local-cli", 2500000);
    const parking::CardPresentedEvent conflicting = makeEvent(
        "reused-event", "20212223", parking::LaneDirection::kEntry,
        parking::CardEventSource::kManualSimulator, "local-cli", 2501000);
    static_cast<void>(coordinator.handleCardPresented(first));
    const parking::AccessResult result =
        coordinator.handleCardPresented(conflicting);

    expectEqual(result.outcome, parking::AccessOutcome::kRejected, name,
                "changed payload reused the cached granted result");
    expectEqual(result.reason, parking::AccessReason::kEventIdConflict, name,
                "changed payload returned the wrong conflict reason");
    expectEqual(repository.sessionCount(), static_cast<std::size_t>(1U), name,
                "event ID collision modified repository state");
}

void testExitAndFee() {
    const std::string name = "exit and fee";
    parking::InMemoryParkingRepository repository;
    parking::FakePlateRecognizer recognizer;
    const parking::FeePolicy fee_policy(600, 3600, 500, 5000);
    parking::ParkingCoordinator coordinator(&repository, &recognizer,
                                            fee_policy);

    const std::int64_t entry_time_ms = 3000000;
    const parking::CardPresentedEvent entry = makeEvent(
        "entry-for-exit", "A1B2C3D4", parking::LaneDirection::kEntry,
        parking::CardEventSource::kSerialRfid, "entry-reader-0",
        entry_time_ms);
    const parking::AccessResult entry_result =
        coordinator.handleCardPresented(entry);
    expectEqual(entry_result.outcome, parking::AccessOutcome::kEntryGranted,
                name, "test setup entry failed");

    const parking::CardPresentedEvent exit = makeEvent(
        "manual-exit-1", "A1B2C3D4", parking::LaneDirection::kExit,
        parking::CardEventSource::kManualSimulator, "local-cli",
        entry_time_ms + 3700 * 1000);
    const parking::AccessResult exit_result =
        coordinator.handleCardPresented(exit);

    expectEqual(exit_result.outcome, parking::AccessOutcome::kExitGranted,
                name, "active card was not granted exit");
    expectEqual(exit_result.parking_duration_seconds,
                static_cast<std::int64_t>(3700), name,
                "parking duration was calculated incorrectly");
    expectEqual(exit_result.fee_cent, static_cast<std::int64_t>(500), name,
                "fee did not apply free time and unit rounding");
    expectEqual(repository.activeSessionCount(),
                static_cast<std::size_t>(0U), name,
                "exit did not close the active session");
    expectEqual(repository.sessionCount(), static_cast<std::size_t>(1U), name,
                "exit deleted history or added another session");
    expectEqual(repository.sessions().front().exit_source,
                parking::CardEventSource::kManualSimulator, name,
                "exit source was not retained");
}

void testRejectedPaths() {
    const std::string name = "rejected paths";
    parking::InMemoryParkingRepository repository;
    parking::FakePlateRecognizer recognizer;
    const parking::FeePolicy fee_policy(600, 3600, 500, 5000);
    parking::ParkingCoordinator coordinator(&repository, &recognizer,
                                            fee_policy);

    const parking::CardPresentedEvent missing_exit = makeEvent(
        "missing-exit", "55667788", parking::LaneDirection::kExit,
        parking::CardEventSource::kManualSimulator, "local-cli", 4000000);
    const parking::AccessResult exit_result =
        coordinator.handleCardPresented(missing_exit);
    expectEqual(exit_result.reason, parking::AccessReason::kNoActiveSession,
                name, "exit without entry was not rejected correctly");

    recognizer.setResult(parking::PlateRecognitionResult(false, "", 0U));
    const parking::CardPresentedEvent failed_entry = makeEvent(
        "failed-lpr", "01020304", parking::LaneDirection::kEntry,
        parking::CardEventSource::kManualSimulator, "local-cli", 5000000);
    const parking::AccessResult recognition_result =
        coordinator.handleCardPresented(failed_entry);
    expectEqual(recognition_result.reason,
                parking::AccessReason::kRecognitionFailed, name,
                "recognition failure did not reject entry");
    expectEqual(repository.sessionCount(), static_cast<std::size_t>(0U), name,
                "rejected paths modified repository state");
}

void testFeeBoundaries() {
    const std::string name = "fee boundaries";
    const parking::FeePolicy fee_policy(600, 3600, 500, 1200);
    expectEqual(fee_policy.calculate(600), static_cast<std::int64_t>(0), name,
                "free boundary was charged");
    expectEqual(fee_policy.calculate(601), static_cast<std::int64_t>(500), name,
                "first billable second did not round up to one unit");
    expectEqual(fee_policy.calculate(100000),
                static_cast<std::int64_t>(1200), name,
                "maximum fee cap was not applied");
}

}  // namespace

int main() {
    try {
        testCardIdNormalization();
        testManualEntryAndIdempotentReplay();
        testDuplicateEntryAcrossSources();
        testEventIdPayloadConflict();
        testExitAndFee();
        testRejectedPaths();
        testFeeBoundaries();
    } catch (const std::exception& error) {
        fail("unexpected exception", error.what());
    }

    if (failure_count != 0) {
        std::cerr << failure_count << " parking domain assertion(s) failed\n";
        return 1;
    }
    std::cout << "All parking domain tests passed\n";
    return 0;
}
