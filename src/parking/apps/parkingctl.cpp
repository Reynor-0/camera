/*
 * 文件用途：实现手动模拟刷卡命令行客户端，连接 parkingd 并打印最终结果。
 * 所属层次：apps，不直接访问 SQLite 或执行业务状态机。
 */
#include "parking/control/protocol.hpp"
#include "parking/control/server.hpp"
#include "parking/domain/card.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

namespace {

const char kDefaultSocketPath[] = "/run/parking-lot/control.sock";

class ScopedFileDescriptor {
public:
    explicit ScopedFileDescriptor(int file_descriptor)
        : file_descriptor_(file_descriptor) {}
    ~ScopedFileDescriptor() {
        if (file_descriptor_ >= 0) {
            close(file_descriptor_);
        }
    }
    ScopedFileDescriptor(const ScopedFileDescriptor&) = delete;
    ScopedFileDescriptor& operator=(const ScopedFileDescriptor&) = delete;

private:
    int file_descriptor_;
};

std::uint64_t monotonicNanoseconds() {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        throw std::runtime_error("clock_gettime(CLOCK_MONOTONIC) failed");
    }
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(value.tv_nsec);
}

std::int64_t monotonicMilliseconds() {
    return static_cast<std::int64_t>(monotonicNanoseconds() / 1000000ULL);
}

int parseWaitMilliseconds(const std::string& text) {
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0' || value < 1L ||
        value > 60000L) {
        throw std::invalid_argument("--wait-ms must be between 1 and 60000");
    }
    return static_cast<int>(value);
}

std::string generateEventId() {
    std::ostringstream stream;
    stream << "manual-" << getpid() << '-' << monotonicNanoseconds();
    return stream.str();
}

void printVersion() {
#ifdef CAMERA_DEMO_TARGET_RK3568_AARCH64
    const char* target = "AArch64/RK3568";
#else
    const char* target = "host";
#endif
    std::cout << "parkingctl " << CAMERA_DEMO_VERSION << " (" << target
              << ", protocol " << parking::kManualControlProtocolVersion
              << ")\n";
}

void printUsage(const char* program) {
    std::cout
        << "Usage:\n"
        << "  " << program
        << " card-present --direction entry|exit --card UID [options]\n"
        << "  " << program << " --help\n"
        << "  " << program << " --version\n\n"
        << "Options:\n"
        << "  --direction entry|exit   required logical lane direction\n"
        << "  --card UID               4/7/10-byte RFID UID in hexadecimal\n"
        << "  --event-id ID            optional idempotency ID (max 64 chars)\n"
        << "  --socket PATH            control socket (default: "
        << kDefaultSocketPath << ")\n"
        << "  --wait-ms N              ACK/FINAL timeout, 1..60000 (default: 5000)\n\n"
        << "Examples:\n"
        << "  " << program
        << " card-present --direction entry --card 04A10B7F\n"
        << "  " << program
        << " card-present --direction exit --card 04A10B7F\n\n"
        << "A successful command injects a manual event into the same business\n"
        << "state machine as real RFID. parkingd persists sessions in SQLite.\n";
}

int connectSocket(const std::string& socket_path) {
    struct sockaddr_un address;
    if (socket_path.empty() || socket_path[0] != '/' ||
        socket_path.size() >= sizeof(address.sun_path)) {
        throw std::invalid_argument("control socket path is invalid");
    }
    const int socket_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (socket_fd < 0) {
        throw std::runtime_error(std::string("socket failed: ") +
                                 std::strerror(errno));
    }
    std::memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket_path.c_str(),
                socket_path.size() + 1U);
    if (connect(socket_fd, reinterpret_cast<struct sockaddr*>(&address),
                sizeof(address)) != 0) {
        const int saved_errno = errno;
        close(socket_fd);
        std::ostringstream message;
        message << "connect '" << socket_path << "' failed: "
                << std::strerror(saved_errno) << " (errno=" << saved_errno
                << ')';
        throw std::runtime_error(message.str());
    }
    return socket_fd;
}

parking::ControlResponse receiveResponse(int socket_fd,
                                         int timeout_ms,
                                         const std::string& event_id) {
    struct pollfd descriptor;
    descriptor.fd = socket_fd;
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    int poll_result;
    do {
        poll_result = poll(&descriptor, 1, timeout_ms);
    } while (poll_result < 0 && errno == EINTR);
    if (poll_result == 0) {
        throw std::runtime_error("TIMEOUT waiting for event " + event_id);
    }
    if (poll_result < 0 || (descriptor.revents & POLLIN) == 0) {
        throw std::runtime_error("control socket failed while waiting for " +
                                 event_id);
    }

    const std::string packet = parking::ManualCardServer::receivePacket(
        socket_fd, parking::kMaximumControlPacketBytes);
    parking::ControlResponse response;
    std::string parse_error;
    if (!parking::parseControlResponse(packet, &response, &parse_error)) {
        throw std::runtime_error("invalid server response: " + parse_error);
    }
    if (response.event_id != event_id && response.event_id != "unknown") {
        throw std::runtime_error("server response event ID mismatch");
    }
    return response;
}

void printResponse(const parking::ControlResponse& response) {
    if (response.type == parking::ControlResponseType::kAck) {
        std::cout << "ACK event_id=" << response.event_id
                  << " accepted=true\n";
    } else if (response.type == parking::ControlResponseType::kReject) {
        std::cout << "REJECT event_id=" << response.event_id
                  << " reason=" << response.reason << '\n';
    } else {
        std::cout << "FINAL event_id=" << response.event_id
                  << " outcome=" << response.outcome
                  << " reason=" << response.reason
                  << " session_id="
                  << (response.session_id.empty() ? "-" : response.session_id)
                  << " plate="
                  << (response.plate_number.empty() ? "-"
                                                    : response.plate_number)
                  << " duration_seconds="
                  << response.parking_duration_seconds
                  << " fee_cent=" << response.fee_cent
                  << " replayed="
                  << (response.replayed ? "true" : "false") << '\n';
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 &&
            (std::string(argv[1]) == "--help" ||
             std::string(argv[1]) == "-h")) {
            printUsage(argv[0]);
            return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--version") {
            printVersion();
            return 0;
        }
        if (argc < 2 || std::string(argv[1]) != "card-present") {
            throw std::invalid_argument("expected the card-present command");
        }

        std::string card_text;
        std::string event_id;
        std::string socket_path = kDefaultSocketPath;
        int wait_ms = 5000;
        bool has_direction = false;
        parking::LaneDirection direction = parking::LaneDirection::kEntry;

        for (int index = 2; index < argc; ++index) {
            const std::string argument(argv[index]);
            if (argument == "-h" || argument == "--help") {
                printUsage(argv[0]);
                return 0;
            }
            if (argument == "--direction" && index + 1 < argc) {
                const std::string value(argv[++index]);
                if (value == "entry") {
                    direction = parking::LaneDirection::kEntry;
                } else if (value == "exit") {
                    direction = parking::LaneDirection::kExit;
                } else {
                    throw std::invalid_argument(
                        "--direction must be entry or exit");
                }
                has_direction = true;
            } else if (argument == "--card" && index + 1 < argc) {
                card_text = argv[++index];
            } else if (argument == "--event-id" && index + 1 < argc) {
                event_id = argv[++index];
            } else if (argument == "--socket" && index + 1 < argc) {
                socket_path = argv[++index];
            } else if (argument == "--wait-ms" && index + 1 < argc) {
                wait_ms = parseWaitMilliseconds(argv[++index]);
            } else {
                throw std::invalid_argument("unknown or incomplete argument: " +
                                            argument);
            }
        }
        if (!has_direction || card_text.empty()) {
            throw std::invalid_argument(
                "--direction and --card are required");
        }

        const parking::CardId card = parking::CardId::fromText(card_text);
        parking::ManualCardRequest request;
        request.event_id = event_id.empty() ? generateEventId() : event_id;
        request.direction = direction;
        request.card_text = card.value();
        const std::string request_packet =
            parking::encodeManualCardRequest(request);

        const int socket_fd = connectSocket(socket_path);
        ScopedFileDescriptor socket_owner(socket_fd);
        parking::ManualCardServer::sendPacket(socket_fd, request_packet);

        const std::int64_t deadline_ms = monotonicMilliseconds() + wait_ms;
        parking::ControlResponse response =
            receiveResponse(socket_fd, wait_ms, request.event_id);
        printResponse(response);
        if (response.type == parking::ControlResponseType::kReject) {
            return 4;
        }
        if (response.type != parking::ControlResponseType::kAck) {
            throw std::runtime_error("expected ACK before FINAL");
        }

        const std::int64_t remaining_ms = deadline_ms - monotonicMilliseconds();
        if (remaining_ms <= 0) {
            throw std::runtime_error("TIMEOUT waiting for final result");
        }
        response = receiveResponse(socket_fd, static_cast<int>(remaining_ms),
                                   request.event_id);
        printResponse(response);
        if (response.type != parking::ControlResponseType::kFinal) {
            throw std::runtime_error("expected FINAL after ACK");
        }
        return 0;
    } catch (const std::invalid_argument& error) {
        std::cerr << "parkingctl: " << error.what() << '\n'
                  << "Try '" << argv[0] << " --help' for usage.\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "parkingctl: " << error.what() << '\n';
        const std::string message(error.what());
        return message.find("TIMEOUT") != std::string::npos ? 5 : 3;
    }
}
