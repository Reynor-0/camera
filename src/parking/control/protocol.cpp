/*
 * 文件用途：实现 PARKING/1 请求、ACK、REJECT 和 FINAL 的编解码。
 * 所属层次：control，不创建 socket，也不修改业务状态。
 */
#include "parking/control/protocol.hpp"

#include <cerrno>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace parking {

const char kManualControlProtocolVersion[] = "PARKING/1";

namespace {

bool containsDelimiter(const std::string& value) {
    return value.find('\t') != std::string::npos ||
           value.find('\n') != std::string::npos ||
           value.find('\r') != std::string::npos;
}

bool isValidEventId(const std::string& event_id) {
    if (event_id.empty() || event_id.size() > 64U ||
        containsDelimiter(event_id)) {
        return false;
    }
    for (std::string::const_iterator it = event_id.begin();
         it != event_id.end(); ++it) {
        const char value = *it;
        const bool allowed =
            (value >= 'a' && value <= 'z') ||
            (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') || value == '-' ||
            value == '_' || value == '.' || value == ':';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> splitFields(const std::string& packet) {
    std::vector<std::string> fields;
    std::string::size_type begin = 0U;
    while (true) {
        const std::string::size_type delimiter = packet.find('\t', begin);
        if (delimiter == std::string::npos) {
            fields.push_back(packet.substr(begin));
            break;
        }
        fields.push_back(packet.substr(begin, delimiter - begin));
        begin = delimiter + 1U;
    }
    return fields;
}

bool parseInt64(const std::string& text, std::int64_t* value) {
    if (text.empty() || value == nullptr) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long long parsed = std::strtoll(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0') {
        return false;
    }
    *value = static_cast<std::int64_t>(parsed);
    return true;
}

std::string safeField(const std::string& value) {
    return value.empty() ? "-" : value;
}

}  // namespace

ManualCardRequest::ManualCardRequest()
    : event_id(), direction(LaneDirection::kEntry), card_text() {}

ControlResponse::ControlResponse()
    : type(ControlResponseType::kReject),
      event_id(),
      outcome(),
      reason(),
      session_id(),
      plate_number(),
      parking_duration_seconds(0),
      fee_cent(0),
      replayed(false) {}

std::string encodeManualCardRequest(const ManualCardRequest& request) {
    if (!isValidEventId(request.event_id)) {
        throw std::invalid_argument("invalid manual event ID");
    }
    if (request.card_text.empty() || containsDelimiter(request.card_text)) {
        throw std::invalid_argument("invalid manual card field");
    }

    std::ostringstream stream;
    stream << kManualControlProtocolVersion << "\tCARD_PRESENT\t"
           << request.event_id << '\t'
           << (request.direction == LaneDirection::kEntry ? "ENTRY" : "EXIT")
           << '\t' << request.card_text;
    const std::string packet = stream.str();
    if (packet.size() > kMaximumControlPacketBytes) {
        throw std::invalid_argument("manual control packet is too large");
    }
    return packet;
}

bool parseManualCardRequest(const std::string& packet,
                            ManualCardRequest* request,
                            std::string* error) noexcept {
    if (request == nullptr || error == nullptr) {
        return false;
    }
    try {
        if (packet.empty() || packet.size() > kMaximumControlPacketBytes) {
            *error = "INVALID_PACKET_SIZE";
            return false;
        }
        const std::vector<std::string> fields = splitFields(packet);
        if (fields.size() != 5U) {
            *error = "INVALID_FIELD_COUNT";
            return false;
        }
        if (fields[0] != kManualControlProtocolVersion) {
            *error = "UNSUPPORTED_VERSION";
            return false;
        }
        if (fields[1] != "CARD_PRESENT") {
            *error = "UNSUPPORTED_COMMAND";
            return false;
        }
        if (!isValidEventId(fields[2])) {
            *error = "INVALID_EVENT_ID";
            return false;
        }
        if (fields[3] == "ENTRY") {
            request->direction = LaneDirection::kEntry;
        } else if (fields[3] == "EXIT") {
            request->direction = LaneDirection::kExit;
        } else {
            *error = "INVALID_DIRECTION";
            return false;
        }
        if (fields[4].empty() || containsDelimiter(fields[4])) {
            *error = "INVALID_CARD_FIELD";
            return false;
        }
        request->event_id = fields[2];
        request->card_text = fields[4];
        error->clear();
        return true;
    } catch (...) {
        *error = "PROTOCOL_PARSE_ERROR";
        return false;
    }
}

std::string encodeControlAck(const std::string& event_id) {
    return std::string(kManualControlProtocolVersion) + "\tACK\t" +
           safeField(event_id) + "\tACCEPTED";
}

std::string encodeControlReject(const std::string& event_id,
                                const std::string& reason) {
    if (containsDelimiter(reason)) {
        throw std::invalid_argument("control reject reason has a delimiter");
    }
    return std::string(kManualControlProtocolVersion) + "\tREJECT\t" +
           safeField(event_id) + '\t' + safeField(reason);
}

std::string encodeControlFinal(const AccessResult& result) {
    std::ostringstream stream;
    stream << kManualControlProtocolVersion << "\tFINAL\t"
           << safeField(result.event_id) << '\t'
           << accessOutcomeName(result.outcome) << '\t'
           << accessReasonName(result.reason) << '\t'
           << safeField(result.session_id) << '\t'
           << safeField(result.plate_number) << '\t'
           << result.parking_duration_seconds << '\t' << result.fee_cent
           << '\t' << (result.replayed ? "1" : "0");
    return stream.str();
}

bool parseControlResponse(const std::string& packet,
                          ControlResponse* response,
                          std::string* error) noexcept {
    if (response == nullptr || error == nullptr) {
        return false;
    }
    try {
        const std::vector<std::string> fields = splitFields(packet);
        if (fields.size() < 2U || fields[0] != kManualControlProtocolVersion) {
            *error = "INVALID_RESPONSE_VERSION";
            return false;
        }
        if (fields[1] == "ACK" && fields.size() == 4U) {
            response->type = ControlResponseType::kAck;
            response->event_id = fields[2];
            response->outcome = fields[3];
        } else if (fields[1] == "REJECT" && fields.size() == 4U) {
            response->type = ControlResponseType::kReject;
            response->event_id = fields[2];
            response->reason = fields[3];
        } else if (fields[1] == "FINAL" && fields.size() == 10U) {
            response->type = ControlResponseType::kFinal;
            response->event_id = fields[2];
            response->outcome = fields[3];
            response->reason = fields[4];
            response->session_id = fields[5] == "-" ? "" : fields[5];
            response->plate_number = fields[6] == "-" ? "" : fields[6];
            if (!parseInt64(fields[7], &response->parking_duration_seconds) ||
                !parseInt64(fields[8], &response->fee_cent) ||
                (fields[9] != "0" && fields[9] != "1")) {
                *error = "INVALID_FINAL_FIELD";
                return false;
            }
            response->replayed = fields[9] == "1";
        } else {
            *error = "INVALID_RESPONSE_TYPE_OR_FIELD_COUNT";
            return false;
        }
        error->clear();
        return true;
    } catch (...) {
        *error = "RESPONSE_PARSE_ERROR";
        return false;
    }
}

}  // namespace parking
