#include "stun.hpp"

#include <algorithm>

namespace xcloud {
namespace {

constexpr uint16_t BindingSuccessResponse = 0x0101;
constexpr uint16_t XorMappedAddressAttribute = 0x0020;
constexpr uint32_t MagicCookie = 0x2112A442;
constexpr size_t HeaderSize = 20;
constexpr size_t MaxStunMessageSize = 4096;

uint16_t readU16(const uint8_t* data) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

uint32_t readU32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

} // namespace

bool parseStunBindingSuccessResponse(
    const uint8_t* packet,
    size_t packetSize,
    const std::array<uint8_t, 12>& expectedTransactionId,
    StunMappedAddress& mappedAddress,
    std::string& error) {
    mappedAddress = {};
    error.clear();
    if (packet == nullptr || packetSize < HeaderSize || packetSize > MaxStunMessageSize) {
        error = "STUN response is missing or outside the supported size limit.";
        return false;
    }
    if ((packet[0] & 0xc0) != 0 || readU16(packet) != BindingSuccessResponse) {
        error = "STUN packet is not a Binding Success Response.";
        return false;
    }

    const size_t attributeBytes = readU16(packet + 2);
    if ((attributeBytes & 3) != 0 ||
        attributeBytes != packetSize - HeaderSize ||
        readU32(packet + 4) != MagicCookie) {
        error = "STUN response has an invalid length or magic cookie.";
        return false;
    }
    if (!std::equal(expectedTransactionId.begin(), expectedTransactionId.end(), packet + 8)) {
        error = "STUN response transaction ID does not match the request.";
        return false;
    }

    StunMappedAddress parsedAddress;
    bool foundMappedAddress = false;
    size_t offset = HeaderSize;
    while (offset < packetSize) {
        if (packetSize - offset < 4) {
            error = "STUN response contains a truncated attribute header.";
            return false;
        }
        const uint16_t attributeType = readU16(packet + offset);
        const size_t valueSize = readU16(packet + offset + 2);
        const size_t paddedValueSize = (valueSize + 3) & ~static_cast<size_t>(3);
        offset += 4;
        if (paddedValueSize > packetSize - offset) {
            error = "STUN response contains a truncated attribute value.";
            return false;
        }

        if (attributeType == XorMappedAddressAttribute) {
            if (foundMappedAddress) {
                error = "STUN response contains duplicate XOR-MAPPED-ADDRESS attributes.";
                return false;
            }
            if (valueSize != 8 || packet[offset] != 0 || packet[offset + 1] != 1) {
                error = "STUN response has an unsupported XOR-MAPPED-ADDRESS value.";
                return false;
            }

            const uint16_t port = static_cast<uint16_t>(
                readU16(packet + offset + 2) ^ static_cast<uint16_t>(MagicCookie >> 16));
            if (port == 0) {
                error = "STUN response contains an invalid mapped port.";
                return false;
            }
            parsedAddress.port = port;
            for (size_t i = 0; i < parsedAddress.ipv4.size(); ++i) {
                parsedAddress.ipv4[i] =
                    packet[offset + 4 + i] ^ packet[4 + i];
            }
            foundMappedAddress = true;
        }
        offset += paddedValueSize;
    }

    if (!foundMappedAddress) {
        error = "STUN response does not contain an XOR-MAPPED-ADDRESS.";
        return false;
    }
    mappedAddress = parsedAddress;
    return true;
}

} // namespace xcloud
