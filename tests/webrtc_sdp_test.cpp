#include "webrtc_sdp.hpp"

#include <cassert>
#include <string>

namespace {

const char* TestSdp =
    "v=0\r\n"
    "o=- 123 2 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0 1\r\n"
    "a=ice-ufrag:sessionUfrag\r\n"
    "a=ice-pwd:sessionPassword\r\n"
    "a=fingerprint:sha-256 AA:BB:CC:DD\r\n"
    "a=setup:actpass\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 96 97\r\n"
    "a=mid:0\r\n"
    "a=rtpmap:96 H264/90000\r\n"
    "a=rtpmap:97 rtx/90000\r\n"
    "a=candidate:1 1 UDP 2130706431 192.0.2.1 5000 typ host generation 0\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
    "a=mid:1\r\n"
    "a=ice-ufrag:audioUfrag\r\n"
    "a=ice-pwd:audioPassword\r\n"
    "a=rtpmap:111 opus/48000/2\r\n";

void testParsesSessionAndMediaAttributes() {
    xcloud::SessionDescription description;
    std::string error;
    assert(xcloud::parseSessionDescription(TestSdp, description, error));
    assert(error.empty());
    assert(description.iceUfrag == "sessionUfrag");
    assert(description.icePassword == "sessionPassword");
    assert(description.fingerprintAlgorithm == "sha-256");
    assert(description.setup == "actpass");
    assert(description.media.size() == 2);

    const auto& video = description.media[0];
    assert(video.media == "video");
    assert(video.mid == "0");
    assert(video.protocol == "UDP/TLS/RTP/SAVPF");
    assert(video.iceUfrag == "sessionUfrag");
    assert(video.icePassword == "sessionPassword");
    assert(video.fingerprint == "AA:BB:CC:DD");
    assert(video.payloadTypes.size() == 2);
    assert(video.codecs.size() == 2);
    assert(video.codecs[0].payloadType == 96);
    assert(video.codecs[0].name == "H264");
    assert(video.codecs[0].clockRate == 90000);
    assert(video.iceCandidates.size() == 1);
    assert(video.iceCandidates[0].address == "192.0.2.1");
    assert(video.iceCandidates[0].port == 5000);
    assert(video.iceCandidates[0].type == "host");

    const auto& audio = description.media[1];
    assert(audio.media == "audio");
    assert(audio.mid == "1");
    assert(audio.iceUfrag == "audioUfrag");
    assert(audio.icePassword == "audioPassword");
    assert(audio.codecs.size() == 1);
    assert(audio.codecs[0].name == "opus");
    assert(audio.codecs[0].clockRate == 48000);
    assert(audio.codecs[0].channels == 2);
}

void testRejectsInvalidPayloadMapping() {
    xcloud::SessionDescription description;
    std::string error;
    const std::string sdp =
        "v=0\n"
        "m=video 9 RTP/AVP 96\n"
        "a=rtpmap:97 H264/90000\n";
    assert(!xcloud::parseSessionDescription(sdp, description, error));
    assert(!error.empty());
    assert(description.media.empty());
}

void testRejectsInvalidFingerprint() {
    xcloud::SessionDescription description;
    std::string error;
    const std::string sdp =
        "v=0\n"
        "a=fingerprint:sha-256 not-a-fingerprint\n"
        "m=video 9 RTP/AVP 96\n";
    assert(!xcloud::parseSessionDescription(sdp, description, error));
    assert(!error.empty());
}

void testRejectsMissingMedia() {
    xcloud::SessionDescription description;
    std::string error;
    assert(!xcloud::parseSessionDescription("v=0\r\ns=-\r\n", description, error));
    assert(error == "SDP contains no media sections.");
}

} // namespace

int main() {
    testParsesSessionAndMediaAttributes();
    testRejectsInvalidPayloadMapping();
    testRejectsInvalidFingerprint();
    testRejectsMissingMedia();
}
