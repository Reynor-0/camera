/*
 * 文件用途：装配停车业务依赖并运行 parkingd 单线程控制事件循环。
 * 所属层次：apps，只处理参数、生命周期、日志和模块装配。
 */
#include "parking/control/protocol.hpp"
#include "parking/control/server.hpp"
#include "parking/domain/card.hpp"
#include "parking/domain/coordinator.hpp"
#include "parking/domain/fee.hpp"
#include "parking/recognition/fake.hpp"
#include "parking/storage/sqlite.hpp"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <time.h>
#include <unistd.h>

namespace {

const char kDefaultSocketPath[] = "/run/parking-lot/control.sock";
const char kDefaultDatabasePath[] =
    "/userdata/parking-lot/db/parking.db";
volatile sig_atomic_t stop_requested = 0;

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

void handleSignal(int signal_number) {
    static_cast<void>(signal_number);
    stop_requested = 1;
}

void installSignalHandlers() {
    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_handler = handleSignal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, nullptr) != 0 ||
        sigaction(SIGTERM, &action, nullptr) != 0 ||
        sigaction(SIGHUP, &action, nullptr) != 0) {
        throw std::runtime_error("sigaction failed");
    }

    struct sigaction ignore_pipe;
    std::memset(&ignore_pipe, 0, sizeof(ignore_pipe));
    ignore_pipe.sa_handler = SIG_IGN;
    sigemptyset(&ignore_pipe.sa_mask);
    if (sigaction(SIGPIPE, &ignore_pipe, nullptr) != 0) {
        throw std::runtime_error("sigaction(SIGPIPE) failed");
    }
}

std::uint64_t monotonicNanoseconds() {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        throw std::runtime_error("clock_gettime(CLOCK_MONOTONIC) failed");
    }
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(value.tv_nsec);
}

std::int64_t realtimeMilliseconds() {
    struct timespec value;
    if (clock_gettime(CLOCK_REALTIME, &value) != 0) {
        throw std::runtime_error("clock_gettime(CLOCK_REALTIME) failed");
    }
    return static_cast<std::int64_t>(value.tv_sec) * 1000LL +
           static_cast<std::int64_t>(value.tv_nsec / 1000000L);
}

std::uint32_t parseConfidence(const std::string& text) {
    char* end = nullptr;
    errno = 0;
    const unsigned long value = std::strtoul(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0' ||
        value > 1000000UL) {
        throw std::invalid_argument(
            "--fake-confidence must be between 0 and 1000000");
    }
    return static_cast<std::uint32_t>(value);
}

void printVersion() {
#ifdef CAMERA_DEMO_TARGET_RK3568_AARCH64
    const char* target = "AArch64/RK3568";
#else
    const char* target = "host";
#endif
    std::cout << "parkingd " << CAMERA_DEMO_VERSION << " (" << target
              << ", protocol " << parking::kManualControlProtocolVersion
              << ")\n";
}

void printUsage(const char* program) {
    std::cout
        << "Usage: " << program << " [options]\n\n"
        << "Run the persistent parking coordinator and manual card socket.\n\n"
        << "Options:\n"
        << "  --socket PATH             Unix SOCK_SEQPACKET path\n"
        << "                            (default: " << kDefaultSocketPath << ")\n"
        << "  --database PATH           SQLite database path\n"
        << "                            (default: " << kDefaultDatabasePath << ")\n"
        << "  --fake-plate TEXT         fake entry plate (default: TEST0001)\n"
        << "  --fake-confidence N       0..1000000 (default: 900000)\n"
        << "  -h, --help                show help without opening a socket\n"
        << "  --version                 show version without opening a socket\n\n"
        << "Sessions and event idempotency records are persisted in SQLite.\n"
        << "No physical barrier is controlled.\n";
}

void logResult(const parking::PeerCredentials& credentials,
               const parking::CardPresentedEvent& event,
               const parking::AccessResult& result) {
    std::cout << "event_id=" << event.event_id
              << " source=" << parking::cardEventSourceName(event.source)
              << " source_instance=" << event.source_instance
              << " peer_pid=" << credentials.process_id
              << " peer_uid=" << credentials.user_id
              << " peer_gid=" << credentials.group_id
              << " direction="
              << (event.direction == parking::LaneDirection::kEntry ? "entry"
                                                                    : "exit")
              << " card=" << event.card.value()
              << " outcome=" << parking::accessOutcomeName(result.outcome)
              << " reason=" << parking::accessReasonName(result.reason)
              << " session_id="
              << (result.session_id.empty() ? "-" : result.session_id)
              << " replayed=" << (result.replayed ? "true" : "false")
              << '\n';
}

void handleClient(int client_fd,
                  const parking::PeerCredentials& credentials,
                  parking::ParkingCoordinator* coordinator) {
    struct pollfd descriptor;
    descriptor.fd = client_fd;
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    int wait_result;
    do {
        wait_result = poll(&descriptor, 1, 2000);
    } while (wait_result < 0 && errno == EINTR && stop_requested == 0);
    if (wait_result == 0) {
        throw std::runtime_error("control client request timed out");
    }
    if (wait_result < 0 || (descriptor.revents & POLLIN) == 0) {
        throw std::runtime_error("control client disconnected before request");
    }

    const std::string packet = parking::ManualCardServer::receivePacket(
        client_fd, parking::kMaximumControlPacketBytes);
    parking::ManualCardRequest request;
    std::string protocol_error;
    if (!parking::parseManualCardRequest(packet, &request, &protocol_error)) {
        parking::ManualCardServer::sendPacket(
            client_fd,
            parking::encodeControlReject("unknown", protocol_error));
        return;
    }

    try {
        const parking::CardId card =
            parking::CardId::fromText(request.card_text);
        parking::ManualCardServer::sendPacket(
            client_fd, parking::encodeControlAck(request.event_id));

        std::ostringstream source_instance;
        source_instance << "local-cli-uid-" << credentials.user_id;
        const parking::CardPresentedEvent event(
            request.event_id, card, request.direction,
            parking::CardEventSource::kManualSimulator,
            source_instance.str(), monotonicNanoseconds(),
            realtimeMilliseconds());
        const parking::AccessResult result =
            coordinator->handleCardPresented(event);
        parking::ManualCardServer::sendPacket(
            client_fd, parking::encodeControlFinal(result));
        logResult(credentials, event, result);
    } catch (const std::invalid_argument&) {
        parking::ManualCardServer::sendPacket(
            client_fd,
            parking::encodeControlReject(request.event_id, "INVALID_CARD"));
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string socket_path = kDefaultSocketPath;
    std::string database_path = kDefaultDatabasePath;
    std::string fake_plate = "TEST0001";
    std::uint32_t fake_confidence = 900000U;

    try {
        for (int index = 1; index < argc; ++index) {
            const std::string argument(argv[index]);
            if (argument == "-h" || argument == "--help") {
                printUsage(argv[0]);
                return 0;
            }
            if (argument == "--version") {
                printVersion();
                return 0;
            }
            if (argument == "--socket" && index + 1 < argc) {
                socket_path = argv[++index];
            } else if (argument == "--database" && index + 1 < argc) {
                database_path = argv[++index];
            } else if (argument == "--fake-plate" && index + 1 < argc) {
                fake_plate = argv[++index];
            } else if (argument == "--fake-confidence" && index + 1 < argc) {
                fake_confidence = parseConfidence(argv[++index]);
            } else {
                throw std::invalid_argument("unknown or incomplete argument: " +
                                            argument);
            }
        }

        installSignalHandlers();
        parking::SQLiteParkingRepository repository(database_path);
        parking::FakePlateRecognizer recognizer;
        recognizer.setResult(parking::PlateRecognitionResult(
            true, fake_plate, fake_confidence));
        const parking::FeePolicy fee_policy(600, 3600, 500, 5000);
        parking::ParkingCoordinator coordinator(&repository, &recognizer,
                                                fee_policy);
        parking::ManualCardServer server(socket_path, 0660U);

        std::cout << "parkingd ready: socket=" << socket_path
                  << " database=" << repository.databasePath()
                  << " sessions=" << repository.sessionCount()
                  << " active=" << repository.activeSessionCount()
                  << " fake_plate=" << fake_plate
                  << " barrier=disabled\n";

        while (stop_requested == 0) {
            struct pollfd descriptor;
            descriptor.fd = server.fileDescriptor();
            descriptor.events = POLLIN;
            descriptor.revents = 0;
            const int result = poll(&descriptor, 1, 500);
            if (result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error(std::string("poll failed: ") +
                                         std::strerror(errno));
            }
            if (result == 0) {
                continue;
            }
            if ((descriptor.revents & POLLIN) == 0) {
                throw std::runtime_error("manual control listener failed");
            }

            parking::PeerCredentials credentials;
            try {
                const int client_fd = server.acceptClient(&credentials);
                ScopedFileDescriptor client(client_fd);
                handleClient(client_fd, credentials, &coordinator);
            } catch (const std::exception& error) {
                std::cerr << "parkingd client error: " << error.what() << '\n';
            }
        }

        std::cout << "parkingd stopping: sessions=" << repository.sessionCount()
                  << " active=" << repository.activeSessionCount() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "parkingd: " << error.what() << '\n'
                  << "Try '" << argv[0] << " --help' for usage.\n";
        return 1;
    }
}
