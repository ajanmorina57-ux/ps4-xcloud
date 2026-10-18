#pragma once

#include <cstddef>
#include <string>

namespace xcloud {

struct WebRtcProbeResult {
    bool peerCreated = false;
    bool gatheringComplete = false;
    bool localDescriptionReady = false;
    size_t localCandidateCount = 0;
    std::string error;
};

WebRtcProbeResult runWebRtcProbe();

} // namespace xcloud
