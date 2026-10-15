#include "webrtc_ice.hpp"

#include <cassert>
#include <string>

namespace {

void testHostUdpCandidate() {
    xcloud::IceCandidate candidate;
    std::string error;
    assert(xcloud::parseIceCandidate(
        "candidate:1 1 UDP 2130706431 192.0.2.10 5000 typ host generation 0",
        candidate,
        error));
    assert(error.empty());
    assert(candidate.foundation == "1");
    assert(candidate.component == 1);
    assert(candidate.transport == "udp");
    assert(candidate.priority == 2130706431);
    assert(candidate.address == "192.0.2.10");
    assert(candidate.port == 5000);
    assert(candidate.type == "host");
}

void testServerReflexiveCandidate() {
    xcloud::IceCandidate candidate;
    std::string error;
    assert(xcloud::parseIceCandidate(
        "candidate:2 1 udp 1694498815 198.51.100.4 62000 typ srflx raddr 10.0.0.4 rport 5000",
        candidate,
        error));
    assert(candidate.type == "srflx");
    assert(candidate.relatedAddress == "10.0.0.4");
    assert(candidate.relatedPort == 5000);
}

void testTcpCandidate() {
    xcloud::IceCandidate candidate;
    std::string error;
    assert(xcloud::parseIceCandidate(
        "candidate:3 1 tcp 1518280447 2001:db8::1 9 typ host tcptype active",
        candidate,
        error));
    assert(candidate.transport == "tcp");
    assert(candidate.tcpType == "active");
    assert(candidate.address == "2001:db8::1");
}

void testRejectsInvalidInput() {
    xcloud::IceCandidate candidate;
    std::string error;

    assert(!xcloud::parseIceCandidate("candidate:1 0 udp 1 192.0.2.1 5000 typ host", candidate, error));
    assert(!error.empty());

    assert(!xcloud::parseIceCandidate("candidate:1 1 udp 1 192.0.2.1 0 typ host", candidate, error));
    assert(!error.empty());

    assert(!xcloud::parseIceCandidate("candidate:1 1 udp 1 192.0.2.1 5000 typ relay", candidate, error));
    assert(!error.empty());

    assert(!xcloud::parseIceCandidate("candidate:1 1 tcp 1 192.0.2.1 5000 typ host", candidate, error));
    assert(!error.empty());
}

} // namespace

int main() {
    testHostUdpCandidate();
    testServerReflexiveCandidate();
    testTcpCandidate();
    testRejectsInvalidInput();
}
