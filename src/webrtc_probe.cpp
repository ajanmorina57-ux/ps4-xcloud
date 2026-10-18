#include "webrtc_probe.hpp"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>

#include "rtc/peerconnection.hpp"

namespace xcloud {
namespace {

struct GatheringStatus {
    std::condition_variable changed;
    std::mutex mutex;
    size_t localCandidateCount = 0;
    bool complete = false;
};

} // namespace

WebRtcProbeResult runWebRtcProbe() {
    WebRtcProbeResult result;
    auto gathering = std::make_shared<GatheringStatus>();
    std::string stage = "libdatachannel setup";

    try {
        rtc::Configuration configuration;
        configuration.disableAutoNegotiation = true;
        stage = "peer construction";
        rtc::PeerConnection peer(configuration);
        result.peerCreated = true;

        peer.onLocalCandidate([gathering](rtc::Candidate) {
            std::lock_guard<std::mutex> lock(gathering->mutex);
            ++gathering->localCandidateCount;
        });
        peer.onGatheringStateChange([gathering](rtc::PeerConnection::GatheringState state) {
            if (state != rtc::PeerConnection::GatheringState::Complete) return;
            {
                std::lock_guard<std::mutex> lock(gathering->mutex);
                gathering->complete = true;
            }
            gathering->changed.notify_all();
        });

        stage = "data channel creation";
        const auto channel = peer.createDataChannel("ps4-transport-probe");
        if (!channel) {
            result.error = "Could not create the local data channel.";
            return result;
        }
        stage = "local SDP and ICE gathering";
        peer.setLocalDescription(rtc::Description::Type::Offer);

        {
            std::unique_lock<std::mutex> lock(gathering->mutex);
            if (!gathering->changed.wait_for(
                    lock,
                    std::chrono::seconds(20),
                    [&gathering] { return gathering->complete; })) {
                result.error = "Timed out waiting for ICE gathering to complete.";
                result.localCandidateCount = gathering->localCandidateCount;
                lock.unlock();
                peer.close();
                return result;
            }
            result.gatheringComplete = gathering->complete;
            result.localCandidateCount = gathering->localCandidateCount;
        }

        result.localDescriptionReady = peer.localDescription().has_value();
        if (!result.localDescriptionReady) {
            result.error = "ICE gathering completed without a local SDP description.";
        } else if (result.localCandidateCount == 0) {
            result.error = "ICE gathering completed without any local candidates.";
        }
        peer.close();
    } catch (const std::exception& error) {
        result.error = stage + ": " + error.what();
    }

    return result;
}

} // namespace xcloud
