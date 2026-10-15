#pragma once

#include <cstdint>
#include <string>

namespace xcloud {

struct IceCandidate {
    std::string foundation;
    uint32_t component = 0;
    std::string transport;
    uint32_t priority = 0;
    std::string address;
    uint16_t port = 0;
    std::string type;
    std::string relatedAddress;
    uint16_t relatedPort = 0;
    std::string tcpType;
};

bool parseIceCandidate(const std::string& attribute, IceCandidate& candidate, std::string& error);

} // namespace xcloud
