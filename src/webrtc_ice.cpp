#include "webrtc_ice.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <vector>

namespace xcloud {
namespace {

bool parseUnsigned(const std::string& value, uint32_t maximum, uint32_t& result) {
    if (value.empty()) return false;
    uint32_t parsed = 0;
    for (const unsigned char character : value) {
        if (!std::isdigit(character)) return false;
        const uint32_t digit = character - '0';
        if (parsed > (maximum - digit) / 10) return false;
        parsed = parsed * 10 + digit;
    }
    result = parsed;
    return true;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

} // namespace

bool parseIceCandidate(const std::string& attribute, IceCandidate& candidate, std::string& error) {
    candidate = {};
    error.clear();

    constexpr const char* CandidatePrefix = "candidate:";
    const std::string candidateValue = attribute.compare(0, 10, CandidatePrefix) == 0
        ? attribute.substr(10)
        : attribute;
    std::istringstream input(candidateValue);
    std::string foundation;
    std::string component;
    std::string transport;
    std::string priority;
    std::string address;
    std::string port;
    std::string typeKeyword;
    std::string type;
    if (!(input >> foundation >> component >> transport >> priority >> address >> port >> typeKeyword >> type) ||
        foundation.empty() || foundation.size() > 32 || typeKeyword != "typ") {
        error = "ICE candidate has an invalid base format.";
        return false;
    }

    uint32_t componentValue = 0;
    uint32_t priorityValue = 0;
    uint32_t portValue = 0;
    if (!parseUnsigned(component, std::numeric_limits<uint32_t>::max(), componentValue) ||
        componentValue == 0 ||
        !parseUnsigned(priority, std::numeric_limits<uint32_t>::max(), priorityValue) ||
        !parseUnsigned(port, 65535, portValue) || portValue == 0) {
        error = "ICE candidate contains an invalid component, priority, or port.";
        return false;
    }

    const std::string normalizedTransport = lower(transport);
    if (normalizedTransport != "udp" && normalizedTransport != "tcp") {
        error = "ICE candidate uses an unsupported transport.";
        return false;
    }
    const std::string normalizedType = lower(type);
    if (normalizedType != "host" && normalizedType != "srflx" &&
        normalizedType != "prflx" && normalizedType != "relay") {
        error = "ICE candidate has an unsupported candidate type.";
        return false;
    }

    IceCandidate parsed;
    parsed.foundation = std::move(foundation);
    parsed.component = componentValue;
    parsed.transport = normalizedTransport;
    parsed.priority = priorityValue;
    parsed.address = std::move(address);
    parsed.port = static_cast<uint16_t>(portValue);
    parsed.type = normalizedType;

    std::string key;
    std::string value;
    while (input >> key) {
        if (!(input >> value)) {
            error = "ICE candidate contains an incomplete extension.";
            return false;
        }
        const std::string normalizedKey = lower(key);
        if (normalizedKey == "raddr") {
            parsed.relatedAddress = value;
        } else if (normalizedKey == "rport") {
            uint32_t relatedPort = 0;
            if (!parseUnsigned(value, 65535, relatedPort) || relatedPort == 0) {
                error = "ICE candidate contains an invalid related port.";
                return false;
            }
            parsed.relatedPort = static_cast<uint16_t>(relatedPort);
        } else if (normalizedKey == "tcptype") {
            parsed.tcpType = lower(value);
        }
    }

    if (parsed.transport == "tcp" &&
        parsed.tcpType != "active" && parsed.tcpType != "passive" && parsed.tcpType != "so") {
        error = "TCP ICE candidate is missing a valid TCP type.";
        return false;
    }
    if (parsed.type == "srflx" || parsed.type == "prflx" || parsed.type == "relay") {
        if (parsed.relatedAddress.empty() || parsed.relatedPort == 0) {
            error = "ICE candidate is missing its related address or port.";
            return false;
        }
    }

    candidate = std::move(parsed);
    return true;
}

} // namespace xcloud
