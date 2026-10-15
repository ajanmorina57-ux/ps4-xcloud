#include "h264_rtp.hpp"

#include <cassert>
#include <cstdint>
#include <vector>

namespace {

std::vector<uint8_t> rtpPacket(
    uint16_t sequence,
    uint32_t timestamp,
    bool marker,
    std::vector<uint8_t> payload) {
    std::vector<uint8_t> packet(12);
    packet[0] = 0x80;
    packet[1] = static_cast<uint8_t>((marker ? 0x80 : 0) | 96);
    packet[2] = static_cast<uint8_t>(sequence >> 8);
    packet[3] = static_cast<uint8_t>(sequence);
    packet[4] = static_cast<uint8_t>(timestamp >> 24);
    packet[5] = static_cast<uint8_t>(timestamp >> 16);
    packet[6] = static_cast<uint8_t>(timestamp >> 8);
    packet[7] = static_cast<uint8_t>(timestamp);
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

void testSingleNal() {
    xcloud::H264RtpDepacketizer depacketizer;
    std::vector<uint8_t> accessUnit;
    const auto packet = rtpPacket(1, 100, true, {0x65, 0xaa});
    assert(depacketizer.pushPacket(packet.data(), packet.size(), accessUnit) ==
           xcloud::H264RtpResult::AccessUnitReady);
    assert((accessUnit == std::vector<uint8_t>{0, 0, 0, 1, 0x65, 0xaa}));
}

void testStapA() {
    xcloud::H264RtpDepacketizer depacketizer;
    std::vector<uint8_t> accessUnit;
    const auto packet = rtpPacket(7, 200, true, {0x78, 0, 2, 0x67, 0x01, 0, 1, 0x68});
    assert(depacketizer.pushPacket(packet.data(), packet.size(), accessUnit) ==
           xcloud::H264RtpResult::AccessUnitReady);
    assert((accessUnit == std::vector<uint8_t>{
        0, 0, 0, 1, 0x67, 0x01,
        0, 0, 0, 1, 0x68}));
}

void testFuA() {
    xcloud::H264RtpDepacketizer depacketizer;
    std::vector<uint8_t> accessUnit;
    const auto start = rtpPacket(8, 300, false, {0x7c, 0x85, 0xaa});
    const auto end = rtpPacket(9, 300, true, {0x7c, 0x45, 0xbb});
    assert(depacketizer.pushPacket(start.data(), start.size(), accessUnit) ==
           xcloud::H264RtpResult::PacketAccepted);
    assert(depacketizer.pushPacket(end.data(), end.size(), accessUnit) ==
           xcloud::H264RtpResult::AccessUnitReady);
    assert((accessUnit == std::vector<uint8_t>{0, 0, 0, 1, 0x65, 0xaa, 0xbb}));
}

void testSequenceGapDropsDamagedAccessUnit() {
    xcloud::H264RtpDepacketizer depacketizer;
    std::vector<uint8_t> accessUnit;
    const auto first = rtpPacket(10, 400, false, {0x65, 0xaa});
    const auto gap = rtpPacket(12, 400, false, {0x65, 0xbb});
    const auto rest = rtpPacket(13, 400, true, {0x65, 0xcc});
    const auto nextFrame = rtpPacket(14, 500, true, {0x65, 0xdd});
    assert(depacketizer.pushPacket(first.data(), first.size(), accessUnit) ==
           xcloud::H264RtpResult::PacketAccepted);
    assert(depacketizer.pushPacket(gap.data(), gap.size(), accessUnit) ==
           xcloud::H264RtpResult::PacketDropped);
    assert(depacketizer.pushPacket(rest.data(), rest.size(), accessUnit) ==
           xcloud::H264RtpResult::PacketDropped);
    assert(depacketizer.pushPacket(nextFrame.data(), nextFrame.size(), accessUnit) ==
           xcloud::H264RtpResult::AccessUnitReady);
    assert((accessUnit == std::vector<uint8_t>{0, 0, 0, 1, 0x65, 0xdd}));
}

void testMalformedHeader() {
    xcloud::H264RtpDepacketizer depacketizer;
    std::vector<uint8_t> accessUnit;
    const std::vector<uint8_t> truncated{0x80, 96, 0, 1};
    assert(depacketizer.pushPacket(truncated.data(), truncated.size(), accessUnit) ==
           xcloud::H264RtpResult::InvalidPacket);
    assert(accessUnit.empty());
}

void testHeaderExtensionAndPadding() {
    xcloud::H264RtpDepacketizer depacketizer;
    std::vector<uint8_t> accessUnit;
    auto packet = rtpPacket(20, 600, true, {0x65, 0xee});
    packet[0] = 0x90;
    packet.insert(packet.begin() + 12, {0xbe, 0xde, 0, 1, 0, 0, 0, 0});
    packet[0] |= 0x20;
    packet.insert(packet.end(), {0, 2});
    assert(depacketizer.pushPacket(packet.data(), packet.size(), accessUnit) ==
           xcloud::H264RtpResult::AccessUnitReady);
    assert((accessUnit == std::vector<uint8_t>{0, 0, 0, 1, 0x65, 0xee}));
}

void testSequenceNumberWraparound() {
    xcloud::H264RtpDepacketizer depacketizer;
    std::vector<uint8_t> accessUnit;
    const auto first = rtpPacket(65535, 700, false, {0x65, 0xaa});
    const auto last = rtpPacket(0, 700, true, {0x65, 0xbb});
    assert(depacketizer.pushPacket(first.data(), first.size(), accessUnit) ==
           xcloud::H264RtpResult::PacketAccepted);
    assert(depacketizer.pushPacket(last.data(), last.size(), accessUnit) ==
           xcloud::H264RtpResult::AccessUnitReady);
    assert((accessUnit == std::vector<uint8_t>{
        0, 0, 0, 1, 0x65, 0xaa,
        0, 0, 0, 1, 0x65, 0xbb}));
}

} // namespace

int main() {
    testSingleNal();
    testStapA();
    testFuA();
    testSequenceGapDropsDamagedAccessUnit();
    testMalformedHeader();
    testHeaderExtensionAndPadding();
    testSequenceNumberWraparound();
}
