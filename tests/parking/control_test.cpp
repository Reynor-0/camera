/*
 * 文件用途：验证 PARKING/1 本地控制协议的编解码和输入拒绝规则。
 */
#include "parking/control/protocol.hpp"
#include "parking/domain/model.hpp"

#include <cstddef>
#include <iostream>
#include <string>

namespace {

int failure_count = 0;

void expectTrue(bool condition,
                const std::string& name,
                const std::string& detail) {
    if (!condition) {
        ++failure_count;
        std::cerr << "[FAIL] " << name << ": " << detail << '\n';
    }
}

template <typename Actual, typename Expected>
void expectEqual(const Actual& actual,
                 const Expected& expected,
                 const std::string& name,
                 const std::string& detail) {
    expectTrue(actual == expected, name, detail);
}

void testRequestRoundTrip() {
    const std::string name = "request round trip";
    parking::ManualCardRequest request;
    request.event_id = "manual-test-1";
    request.direction = parking::LaneDirection::kEntry;
    request.card_text = "04A10B7F";

    const std::string packet = parking::encodeManualCardRequest(request);
    parking::ManualCardRequest parsed;
    std::string error;
    expectTrue(parking::parseManualCardRequest(packet, &parsed, &error), name,
               "encoded request could not be parsed: " + error);
    expectEqual(parsed.event_id, request.event_id, name,
                "event ID changed during round trip");
    expectEqual(parsed.direction, request.direction, name,
                "direction changed during round trip");
    expectEqual(parsed.card_text, request.card_text, name,
                "card changed during round trip");
}

void testRequestRejections() {
    const std::string name = "request rejection";
    parking::ManualCardRequest parsed;
    std::string error;
    expectTrue(!parking::parseManualCardRequest(
                   "PARKING/2\tCARD_PRESENT\tevent-1\tENTRY\t01020304",
                   &parsed, &error),
               name, "unsupported version was accepted");
    expectEqual(error, std::string("UNSUPPORTED_VERSION"), name,
                "wrong version rejection reason");

    expectTrue(!parking::parseManualCardRequest(
                   "PARKING/1\tCARD_PRESENT\tevent-1\tSIDEWAYS\t01020304",
                   &parsed, &error),
               name, "invalid direction was accepted");
    expectEqual(error, std::string("INVALID_DIRECTION"), name,
                "wrong direction rejection reason");
}

void testResponses() {
    const std::string name = "response parsing";
    parking::ControlResponse response;
    std::string error;
    expectTrue(parking::parseControlResponse(
                   parking::encodeControlAck("event-2"), &response, &error),
               name, "ACK could not be parsed");
    expectEqual(response.type, parking::ControlResponseType::kAck, name,
                "ACK type changed");

    const parking::AccessResult source_result(
        parking::AccessOutcome::kEntryGranted, parking::AccessReason::kNone);
    parking::AccessResult result = source_result;
    result.event_id = "event-2";
    result.session_id = "session-00000001";
    result.plate_number = "TEST1234";
    const std::string final_packet = parking::encodeControlFinal(result);
    response = parking::ControlResponse();
    expectTrue(parking::parseControlResponse(final_packet, &response, &error),
               name, "FINAL could not be parsed: " + error);
    expectEqual(response.type, parking::ControlResponseType::kFinal, name,
                "FINAL type changed");
    expectEqual(response.outcome, std::string("ENTRY_GRANTED"), name,
                "FINAL outcome changed");
    expectEqual(response.session_id, result.session_id, name,
                "FINAL session ID changed");
}

void testOversizedPacket() {
    const std::string name = "packet size limit";
    const std::string packet(parking::kMaximumControlPacketBytes + 1U, 'A');
    parking::ManualCardRequest request;
    std::string error;
    expectTrue(!parking::parseManualCardRequest(packet, &request, &error), name,
               "oversized request was accepted");
    expectEqual(error, std::string("INVALID_PACKET_SIZE"), name,
                "oversized request returned the wrong reason");
}

}  // namespace

int main() {
    testRequestRoundTrip();
    testRequestRejections();
    testResponses();
    testOversizedPacket();
    if (failure_count != 0) {
        std::cerr << failure_count << " control protocol assertion(s) failed\n";
        return 1;
    }
    std::cout << "All parking control protocol tests passed\n";
    return 0;
}
