#pragma once

#include "webrtc_ice.hpp"

#include <string>
#include <vector>

namespace xcloud {

struct WebRtcSignaledCandidate {
    std::string candidate;
    std::string sdpMid;
    int sdpMLineIndex = 0;
};

struct SdpCodec {
    int payloadType = -1;
    std::string name;
    unsigned int clockRate = 0;
    unsigned int channels = 1;
};

struct SdpMediaSection {
    std::string media;
    std::string protocol;
    std::string mid;
    unsigned int port = 0;
    std::string iceUfrag;
    std::string icePassword;
    std::string fingerprintAlgorithm;
    std::string fingerprint;
    std::string setup;
    std::vector<int> payloadTypes;
    std::vector<SdpCodec> codecs;
    std::vector<IceCandidate> iceCandidates;
};

struct SessionDescription {
    std::string iceUfrag;
    std::string icePassword;
    std::string fingerprintAlgorithm;
    std::string fingerprint;
    std::string setup;
    std::vector<SdpMediaSection> media;
};

bool parseSessionDescription(
    const std::string& sdp,
    SessionDescription& description,
    std::string& error);

} // namespace xcloud
