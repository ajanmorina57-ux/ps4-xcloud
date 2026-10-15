#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace xcloud {

enum class H264RtpResult {
    PacketAccepted,
    AccessUnitReady,
    PacketDropped,
    InvalidPacket,
    AccessUnitTooLarge
};

class H264RtpDepacketizer {
public:
    static constexpr size_t MaxAccessUnitSize = 8 * 1024 * 1024;

    H264RtpResult pushPacket(
        const uint8_t* packet,
        size_t packetSize,
        std::vector<uint8_t>& accessUnit);
    void reset();

private:
    H264RtpResult appendNal(const uint8_t* data, size_t size);
    H264RtpResult dropCurrentAccessUnit();

    std::vector<uint8_t> m_accessUnit;
    uint32_t m_timestamp = 0;
    uint16_t m_nextSequence = 0;
    bool m_haveTimestamp = false;
    bool m_haveSequence = false;
    bool m_fragmentOpen = false;
    bool m_discardTimestamp = false;
};

} // namespace xcloud
