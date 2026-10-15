#include "h264_rtp.hpp"

namespace xcloud {
namespace {

constexpr uint8_t AnnexBStartCode[] = {0x00, 0x00, 0x00, 0x01};

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

void H264RtpDepacketizer::reset() {
    m_accessUnit.clear();
    m_timestamp = 0;
    m_nextSequence = 0;
    m_haveTimestamp = false;
    m_haveSequence = false;
    m_fragmentOpen = false;
    m_discardTimestamp = false;
}

H264RtpResult H264RtpDepacketizer::appendNal(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) return dropCurrentAccessUnit();
    if (size > MaxAccessUnitSize - sizeof(AnnexBStartCode) ||
        m_accessUnit.size() > MaxAccessUnitSize - sizeof(AnnexBStartCode) - size) {
        reset();
        return H264RtpResult::AccessUnitTooLarge;
    }
    m_accessUnit.insert(
        m_accessUnit.end(),
        AnnexBStartCode,
        AnnexBStartCode + sizeof(AnnexBStartCode));
    m_accessUnit.insert(m_accessUnit.end(), data, data + size);
    return H264RtpResult::PacketAccepted;
}

H264RtpResult H264RtpDepacketizer::dropCurrentAccessUnit() {
    m_accessUnit.clear();
    m_fragmentOpen = false;
    m_discardTimestamp = true;
    return H264RtpResult::PacketDropped;
}

H264RtpResult H264RtpDepacketizer::pushPacket(
    const uint8_t* packet,
    size_t packetSize,
    std::vector<uint8_t>& accessUnit) {
    accessUnit.clear();
    if (packet == nullptr || packetSize < 12) return H264RtpResult::InvalidPacket;

    const uint8_t version = packet[0] >> 6;
    const bool hasPadding = (packet[0] & 0x20) != 0;
    const bool hasExtension = (packet[0] & 0x10) != 0;
    const size_t csrcCount = packet[0] & 0x0f;
    if (version != 2) return H264RtpResult::InvalidPacket;

    size_t payloadOffset = 12 + csrcCount * 4;
    if (payloadOffset > packetSize) return H264RtpResult::InvalidPacket;
    if (hasExtension) {
        if (packetSize - payloadOffset < 4) return H264RtpResult::InvalidPacket;
        const size_t extensionSize = static_cast<size_t>(readU16(packet + payloadOffset + 2)) * 4;
        if (extensionSize > packetSize - payloadOffset - 4) return H264RtpResult::InvalidPacket;
        payloadOffset += 4 + extensionSize;
    }

    size_t payloadEnd = packetSize;
    if (hasPadding) {
        const size_t paddingSize = packet[packetSize - 1];
        if (paddingSize == 0 || paddingSize > payloadEnd - payloadOffset) {
            return H264RtpResult::InvalidPacket;
        }
        payloadEnd -= paddingSize;
    }
    if (payloadOffset >= payloadEnd) return H264RtpResult::InvalidPacket;

    const uint16_t sequence = readU16(packet + 2);
    const uint32_t timestamp = readU32(packet + 4);
    const bool marker = (packet[1] & 0x80) != 0;
    const uint8_t* payload = packet + payloadOffset;
    const size_t payloadSize = payloadEnd - payloadOffset;

    if (m_haveSequence && sequence != m_nextSequence) {
        m_timestamp = timestamp;
        m_haveTimestamp = true;
        m_nextSequence = static_cast<uint16_t>(sequence + 1);
        m_haveSequence = true;
        m_discardTimestamp = !marker;
        m_accessUnit.clear();
        m_fragmentOpen = false;
        return H264RtpResult::PacketDropped;
    }
    m_nextSequence = static_cast<uint16_t>(sequence + 1);
    m_haveSequence = true;

    if (m_haveTimestamp && timestamp != m_timestamp) {
        m_accessUnit.clear();
        m_fragmentOpen = false;
        m_discardTimestamp = false;
    }
    if (!m_haveTimestamp || timestamp != m_timestamp) {
        m_timestamp = timestamp;
        m_haveTimestamp = true;
    }
    if (m_discardTimestamp) {
        if (!marker) return H264RtpResult::PacketDropped;
        m_discardTimestamp = false;
        return H264RtpResult::PacketDropped;
    }

    const uint8_t nalType = payload[0] & 0x1f;
    H264RtpResult result = H264RtpResult::InvalidPacket;

    if (nalType >= 1 && nalType <= 23) {
        if (m_fragmentOpen) return dropCurrentAccessUnit();
        result = appendNal(payload, payloadSize);
    } else if (nalType == 24) {
        if (m_fragmentOpen) return dropCurrentAccessUnit();
        size_t offset = 1;
        bool hasNal = false;
        while (offset < payloadSize) {
            if (payloadSize - offset < 2) return dropCurrentAccessUnit();
            const size_t nalSize = readU16(payload + offset);
            offset += 2;
            if (nalSize == 0 || nalSize > payloadSize - offset) return dropCurrentAccessUnit();
            const uint8_t stapNalType = payload[offset] & 0x1f;
            if (stapNalType == 0 || stapNalType >= 24) return dropCurrentAccessUnit();
            result = appendNal(payload + offset, nalSize);
            if (result != H264RtpResult::PacketAccepted) return result;
            offset += nalSize;
            hasNal = true;
        }
        if (offset != payloadSize || !hasNal) return dropCurrentAccessUnit();
    } else if (nalType == 28) {
        if (payloadSize < 3) return dropCurrentAccessUnit();
        const uint8_t fuHeader = payload[1];
        const bool start = (fuHeader & 0x80) != 0;
        const bool end = (fuHeader & 0x40) != 0;
        const uint8_t fragmentType = fuHeader & 0x1f;
        if ((fuHeader & 0x20) != 0 || (start && end) ||
            fragmentType == 0 || fragmentType >= 24) {
            return dropCurrentAccessUnit();
        }

        if (start) {
            if (m_fragmentOpen) return dropCurrentAccessUnit();
            const uint8_t reconstructedHeader =
                static_cast<uint8_t>((payload[0] & 0xe0) | fragmentType);
            result = appendNal(&reconstructedHeader, 1);
            if (result != H264RtpResult::PacketAccepted) return result;
            m_fragmentOpen = true;
        } else if (!m_fragmentOpen) {
            return dropCurrentAccessUnit();
        }

        const size_t fragmentSize = payloadSize - 2;
        if (fragmentSize > MaxAccessUnitSize - m_accessUnit.size()) {
            reset();
            return H264RtpResult::AccessUnitTooLarge;
        }
        m_accessUnit.insert(m_accessUnit.end(), payload + 2, payload + payloadSize);
        if (end) m_fragmentOpen = false;
        result = H264RtpResult::PacketAccepted;
    } else {
        return dropCurrentAccessUnit();
    }

    if (result != H264RtpResult::PacketAccepted) return result;
    if (!marker) return H264RtpResult::PacketAccepted;
    if (m_fragmentOpen || m_accessUnit.empty()) return dropCurrentAccessUnit();

    accessUnit.swap(m_accessUnit);
    m_accessUnit.clear();
    m_haveTimestamp = false;
    m_discardTimestamp = false;
    return H264RtpResult::AccessUnitReady;
}

} // namespace xcloud
