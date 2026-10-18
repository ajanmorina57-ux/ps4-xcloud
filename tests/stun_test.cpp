#include "stun.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

namespace {

void appendU16(std::vector<uint8_t>& packet, uint16_t value) {
    packet.push_back(static_cast<uint8_t>(value >> 8));
    packet.push_back(static_cast<uint8_t>(value));
}

std::vector<uint8_t> bindingSuccess(
    const std::array<uint8_t, 12>& transactionId,
    const std::array<uint8_t, 4>& address,
    uint16_t port) {
    std::vector<uint8_t> packet{
        0x01, 0x01, 0x00, 0x0c,
        0x21, 0x12, 0xa4, 0x42};
    packet.insert(packet.end(), transactionId.begin(), transactionId.end());
    appendU16(packet, 0x0020);
    appendU16(packet, 8);
    packet.push_back(0);
    packet.push_back(1);
    appendU16(packet, static_cast<uint16_t>(port ^ 0x2112));
    for (size_t i = 0; i < address.size(); ++i) {
        packet.push_back(address[i] ^ packet[4 + i]);
    }
    return packet;
}

void testParsesXorMappedAddress() {
    const std::array<uint8_t, 12> transactionId{
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
        0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b};
    const std::array<uint8_t, 4> expectedAddress{203, 0, 113, 9};
    const auto packet = bindingSuccess(transactionId, expectedAddress, 3478);
    xcloud::StunMappedAddress mappedAddress;
    std::string error;
    assert(xcloud::parseStunBindingSuccessResponse(
        packet.data(), packet.size(), transactionId, mappedAddress, error));
    assert(error.empty());
    assert(mappedAddress.ipv4 == expectedAddress);
    assert(mappedAddress.port == 3478);
}

void testRejectsWrongTransactionAndMalformedLength() {
    const std::array<uint8_t, 12> transactionId{};
    auto packet = bindingSuccess(transactionId, {192, 0, 2, 1}, 5000);
    std::array<uint8_t, 12> wrongTransaction{};
    wrongTransaction[0] = 1;
    xcloud::StunMappedAddress mappedAddress;
    std::string error;
    assert(!xcloud::parseStunBindingSuccessResponse(
        packet.data(), packet.size(), wrongTransaction, mappedAddress, error));
    assert(!error.empty());

    packet[3] = 0x0b;
    assert(!xcloud::parseStunBindingSuccessResponse(
        packet.data(), packet.size(), transactionId, mappedAddress, error));
    assert(!error.empty());
}

void testRejectsUnsupportedAddressAndMissingAttribute() {
    const std::array<uint8_t, 12> transactionId{};
    auto packet = bindingSuccess(transactionId, {192, 0, 2, 1}, 5000);
    packet[packet.size() - 8] = 2;
    xcloud::StunMappedAddress mappedAddress;
    std::string error;
    assert(!xcloud::parseStunBindingSuccessResponse(
        packet.data(), packet.size(), transactionId, mappedAddress, error));
    assert(!error.empty());

    packet = {0x01, 0x01, 0x00, 0x00, 0x21, 0x12, 0xa4, 0x42};
    packet.insert(packet.end(), transactionId.begin(), transactionId.end());
    assert(!xcloud::parseStunBindingSuccessResponse(
        packet.data(), packet.size(), transactionId, mappedAddress, error));
    assert(!error.empty());
}

void testRejectsDuplicateMappedAddress() {
    const std::array<uint8_t, 12> transactionId{};
    auto packet = bindingSuccess(transactionId, {192, 0, 2, 1}, 5000);
    const std::vector<uint8_t> attribute(packet.begin() + 20, packet.end());
    packet.insert(packet.end(), attribute.begin(), attribute.end());
    packet[2] = 0;
    packet[3] = 24;
    xcloud::StunMappedAddress mappedAddress;
    std::string error;
    assert(!xcloud::parseStunBindingSuccessResponse(
        packet.data(), packet.size(), transactionId, mappedAddress, error));
    assert(!error.empty());
    assert(mappedAddress.port == 0);
    assert((mappedAddress.ipv4 == std::array<uint8_t, 4>{}));
}

} // namespace

int main() {
    testParsesXorMappedAddress();
    testRejectsWrongTransactionAndMalformedLength();
    testRejectsUnsupportedAddressAndMissingAttribute();
    testRejectsDuplicateMappedAddress();
}
