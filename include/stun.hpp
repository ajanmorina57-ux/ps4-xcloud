#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace xcloud {

struct StunMappedAddress {
    std::array<uint8_t, 4> ipv4{};
    uint16_t port = 0;
};

bool parseStunBindingSuccessResponse(
    const uint8_t* packet,
    size_t packetSize,
    const std::array<uint8_t, 12>& expectedTransactionId,
    StunMappedAddress& mappedAddress,
    std::string& error);

} // namespace xcloud
