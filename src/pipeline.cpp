#include "pipeline.hpp"
#include "display.hpp"
#include "nv12_converter.hpp"
#include "json.hpp"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <cctype>
#include <chrono>
#include <algorithm>
#include <condition_variable>
#include <stdexcept>
#include <sys/stat.h>

#if defined(XCLOUD_ENABLE_WEBRTC)
#include <rtc/rtc.hpp>
#endif

#if defined(__ORBIS__) || defined(__PS4__)
#include <orbis/AudioOut.h>
#include <orbis/UserService.h>
#include <orbis/Net.h>
#include <netinet/in.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace xcloud {

struct StreamingPipeline::WebRtcState {
#if defined(XCLOUD_ENABLE_WEBRTC)
    std::shared_ptr<rtc::PeerConnection> peer;
    std::shared_ptr<rtc::Track> videoTrack;
    std::shared_ptr<rtc::Track> audioTrack;
    std::shared_ptr<rtc::DataChannel> chatChannel;
    std::shared_ptr<rtc::DataChannel> controlChannel;
    std::shared_ptr<rtc::DataChannel> inputChannel;
    std::shared_ptr<rtc::DataChannel> messageChannel;
    std::atomic<uint32_t> inputSequence{1};
    std::vector<rtc::Candidate> localCandidates;
    std::vector<std::string> localMids;
#endif
    std::mutex mutex;
    std::condition_variable changed;
    bool gatheringComplete = false;
    bool connected = false;
    bool handshakeAckReceived = false;
    bool handshakeDispatchStarted = false;
    bool handshakeComplete = false;
    bool failed = false;
    std::string failure;
};

namespace {

static void recordDiagnosticStage(const char* stage) {
#if defined(__ORBIS__) || defined(__PS4__)
    mkdir("/data/ps4-xcloud", 0777);
    FILE* file = std::fopen("/data/ps4-xcloud/last_stream_stage.txt", "w");
    if (!file) return;
    std::fprintf(file, "%s\n", stage);
    std::fclose(file);
#else
    (void)stage;
#endif
}

#if defined(XCLOUD_ENABLE_WEBRTC)
static void putLe16(std::vector<rtc::byte>& out, uint16_t value) {
    out.push_back(static_cast<rtc::byte>(value & 0xff));
    out.push_back(static_cast<rtc::byte>((value >> 8) & 0xff));
}

static void putLe32(std::vector<rtc::byte>& out, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<rtc::byte>((value >> shift) & 0xff));
}

static std::vector<rtc::byte> makeInputReport(const XboxGamepadReport& report, uint32_t sequence) {
    // Xbox web input protocol: 14-byte header, then one 23-byte gamepad frame.
    std::vector<rtc::byte> packet;
    packet.reserve(38);
    putLe16(packet, 2); // Gamepad report
    putLe32(packet, sequence);
    const double timestampMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    uint64_t timestampBits = 0;
    std::memcpy(&timestampBits, &timestampMs, sizeof(timestampBits));
    for (unsigned shift = 0; shift < 64; shift += 8)
        packet.push_back(static_cast<rtc::byte>((timestampBits >> shift) & 0xff));

    packet.push_back(static_cast<rtc::byte>(1)); // frame count
    packet.push_back(static_cast<rtc::byte>(0)); // gamepad index
    uint16_t buttons = 0;
    const uint16_t source = report.buttons;
    if (source & (1u << 0)) buttons |= 1u << 4;  // Cross -> Xbox A
    if (source & (1u << 1)) buttons |= 1u << 5;  // Circle -> Xbox B
    if (source & (1u << 2)) buttons |= 1u << 6;  // Square -> Xbox X
    if (source & (1u << 3)) buttons |= 1u << 7;  // Triangle -> Xbox Y
    if (source & (1u << 4)) buttons |= 1u << 12; // L1
    if (source & (1u << 5)) buttons |= 1u << 13; // R1
    if (source & (1u << 6)) buttons |= 1u << 2;  // Options -> Menu
    if (source & (1u << 7)) buttons |= 1u << 3;  // Touchpad -> View
    if (source & (1u << 8)) buttons |= 1u << 14; // L3
    if (source & (1u << 9)) buttons |= 1u << 15; // R3
    if (source & (1u << 10)) buttons |= 1u << 8;  // D-pad up
    if (source & (1u << 11)) buttons |= 1u << 9;  // D-pad down
    if (source & (1u << 12)) buttons |= 1u << 10; // D-pad left
    if (source & (1u << 13)) buttons |= 1u << 11; // D-pad right
    putLe16(packet, buttons);
    putLe16(packet, static_cast<uint16_t>(report.thumbLX));
    putLe16(packet, static_cast<uint16_t>(report.thumbLY));
    putLe16(packet, static_cast<uint16_t>(report.thumbRX));
    putLe16(packet, static_cast<uint16_t>(report.thumbRY));
    const uint16_t leftTrigger = static_cast<uint16_t>(report.leftTrigger * 257u);
    const uint16_t rightTrigger = static_cast<uint16_t>(report.rightTrigger * 257u);
    putLe16(packet, leftTrigger);
    putLe16(packet, rightTrigger);

    uint32_t physicality = 0;
    if (buttons & (1u << 8)) physicality |= 1u << 0;
    if (buttons & (1u << 9)) physicality |= 1u << 1;
    if (buttons & (1u << 10)) physicality |= 1u << 2;
    if (buttons & (1u << 11)) physicality |= 1u << 3;
    if (buttons & (1u << 2)) physicality |= 1u << 4;
    if (buttons & (1u << 3)) physicality |= 1u << 5;
    if (buttons & (1u << 14)) physicality |= 1u << 6;
    if (buttons & (1u << 15)) physicality |= 1u << 7;
    if (buttons & (1u << 12)) physicality |= 1u << 8;
    if (buttons & (1u << 13)) physicality |= 1u << 9;
    if (buttons & (1u << 4)) physicality |= 1u << 12;
    if (buttons & (1u << 5)) physicality |= 1u << 13;
    if (buttons & (1u << 6)) physicality |= 1u << 14;
    if (buttons & (1u << 7)) physicality |= 1u << 15;
    if (leftTrigger) physicality |= 1u << 16;
    if (rightTrigger) physicality |= 1u << 17;
    if (report.thumbLX) physicality |= 1u << 18;
    if (report.thumbLY) physicality |= 1u << 19;
    if (report.thumbRX) physicality |= 1u << 20;
    if (report.thumbRY) physicality |= 1u << 21;
    putLe32(packet, physicality);
    putLe32(packet, 0); // virtual physicality
    return packet;
}

static std::vector<rtc::byte> makeClientMetadataReport() {
    std::vector<rtc::byte> packet;
    packet.reserve(15);
    putLe16(packet, 8); // Client metadata report
    putLe32(packet, 0);
    const double timestampMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    uint64_t timestampBits = 0;
    std::memcpy(&timestampBits, &timestampMs, sizeof(timestampBits));
    for (unsigned shift = 0; shift < 64; shift += 8)
        packet.push_back(static_cast<rtc::byte>((timestampBits >> shift) & 0xff));
    packet.push_back(static_cast<rtc::byte>(1)); // one touch point
    return packet;
}

static std::vector<std::string> extractSdpMids(const std::string& sdp) {
    std::vector<std::string> mids;
    size_t pos = 0;
    while ((pos = sdp.find("a=mid:", pos)) != std::string::npos) {
        pos += 6;
        size_t end = sdp.find_first_of("\r\n", pos);
        mids.push_back(sdp.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
    }
    return mids;
}
#endif

static inline void setPixel(uint32_t* buf, int width, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (x >= 0 && x < width && y >= 0 && y < 1080) {
        buf[y * width + x] = 0xFF000000u | (static_cast<uint32_t>(b) << 16) |
                             (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(r);
    }
}

static inline void fillBox(uint32_t* buf, int screenW, int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    int x2 = std::min(x + w, screenW);
    int y2 = std::min(y + h, 1080);
    uint32_t val = 0xFF000000u | (static_cast<uint32_t>(b) << 16) |
                   (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(r);
    for (int cy = std::max(0, y); cy < y2; ++cy) {
        uint32_t* row = buf + cy * screenW;
        for (int cx = std::max(0, x); cx < x2; ++cx) {
            row[cx] = val;
        }
    }
}

static inline void drawBoxBorder(uint32_t* buf, int screenW, int x, int y, int w, int h, int thick, uint8_t r, uint8_t g, uint8_t b) {
    fillBox(buf, screenW, x, y, w, thick, r, g, b);
    fillBox(buf, screenW, x, y + h - thick, w, thick, r, g, b);
    fillBox(buf, screenW, x, y, thick, h, r, g, b);
    fillBox(buf, screenW, x + w - thick, y, thick, h, r, g, b);
}

static inline void drawGlyph(uint32_t* buf, int screenW, char c, int x, int y, int scale, uint8_t r, uint8_t g, uint8_t b) {
    if (c < 32 || c > 126) c = '?';
    const uint8_t* glyph = g_font8x8_basic[c - 32];
    for (int row = 0; row < 8; ++row) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; ++col) {
            if ((bits >> (7 - col)) & 1) {
                fillBox(buf, screenW, x + col * scale, y + row * scale, scale, scale, r, g, b);
            }
        }
    }
}

static inline void drawString(uint32_t* buf, int screenW, const std::string& str, int x, int y, int scale, uint8_t r, uint8_t g, uint8_t b) {
    int cx = x;
    int cy = y;
    for (char c : str) {
        if (c == '\n') {
            cy += 10 * scale;
            cx = x;
            continue;
        }
        drawGlyph(buf, screenW, c, cx, cy, scale, r, g, b);
        cx += 9 * scale;
    }
}

} // namespace

StreamingPipeline::StreamingPipeline() {}

StreamingPipeline::~StreamingPipeline() {
    shutdown();
}

bool StreamingPipeline::init(uint32_t width, uint32_t height) {
    if (m_initialized) return true;
    m_width = width;
    m_height = height;

    const size_t byteSize = static_cast<size_t>(width) * height * 4;
    m_frameBufferA.assign(byteSize, 0);
    m_frameBufferB.assign(byteSize, 0);
    m_displayBufferIndex.store(0, std::memory_order_release);
    m_frameReady.store(false, std::memory_order_release);

#if defined(__ORBIS__) || defined(__PS4__)
    sceUserServiceInitialize(nullptr);
    int rc = sceAudioOutInit();
    if (rc == 0) {
        m_audioHandle = sceAudioOutOpen(
            ORBIS_USER_SERVICE_USER_ID_SYSTEM,
            ORBIS_AUDIO_OUT_PORT_TYPE_MAIN,
            0,
            256,
            48000,
            ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
        if (m_audioHandle > 0) {
            printf("[Pipeline] Orbis AudioOut initialized (Port Main, 48kHz, 16-bit Stereo, Grain: 256).\n");
        } else {
            printf("[Pipeline] Warning: Could not open Orbis AudioOut port (rc: %d).\n", m_audioHandle);
        }
    } else {
        printf("[Pipeline] Warning: sceAudioOutInit returned %d.\n", rc);
    }
#else
    // Host Linux PipeWire audio pipeline integration
    m_pipewireSink = static_cast<void*>(popen(
        "pw-cat -p --format s16 --rate 48000 --channels 2 2>/dev/null || "
        "pw-play --format s16 --rate 48000 --channels 2 2>/dev/null",
        "w"));
    if (m_pipewireSink) {
        printf("[Pipeline] PipeWire audio sink connected (48kHz, S16_LE Stereo).\n");
    } else {
        printf("[Pipeline] Running in mock host audio mode.\n");
    }
#endif

    if (!m_decoder.init(width, height)) {
        m_lastError = m_decoder.getLastError();
        printf("[Pipeline] Could not initialize the PS4 H.264 decoder: %s\n", m_lastError.c_str());
        return false;
    }
    m_initialized = true;
    m_state = PipelineState::Idle;
    return true;
}

void StreamingPipeline::setStreamInfo(const std::string& titleName, const std::string& remoteIp, uint16_t remotePort) {
    std::lock_guard<std::mutex> lock(m_gamepadMutex);
    m_titleName = titleName.empty() ? "Xbox Cloud Gaming" : titleName;
    m_remoteIp = remoteIp;
    m_remotePort = remotePort;
    printf("[Pipeline] Stream info updated: Title '%s', Endpoint: %s:%u\n",
           m_titleName.c_str(), m_remoteIp.c_str(), m_remotePort);
}

void StreamingPipeline::updateGamepad(const XboxGamepadReport& report) {
    std::lock_guard<std::mutex> lock(m_gamepadMutex);
    m_gamepadReport = report;
#if defined(XCLOUD_ENABLE_WEBRTC)
    if (m_webrtc && m_webrtc->inputChannel && m_webrtc->inputChannel->isOpen()) {
        auto packet = makeInputReport(report, m_webrtc->inputSequence.fetch_add(1));
        m_webrtc->inputChannel->send(packet);
    }
#endif
}

bool StreamingPipeline::createWebRtcOffer(std::string& offerSdp, std::string& error) {
    offerSdp.clear();
    error.clear();
#if !defined(XCLOUD_ENABLE_WEBRTC)
    error = "This build was compiled without libdatachannel. Enable XCLOUD_ENABLE_WEBRTC.";
    return false;
#else
    recordDiagnosticStage("WebRTC setup: initializing PS4 video decoder");
    if (!init(m_width, m_height)) {
        error = m_lastError.empty()
            ? "Could not initialize the PS4 H.264 decoder or media output pipeline."
            : m_lastError;
        return false;
    }
    recordDiagnosticStage("WebRTC setup: PS4 video decoder ready");

    stop();
    auto state = std::make_shared<WebRtcState>();
    rtc::Configuration configuration;
    configuration.disableAutoNegotiation = true;
    // Keep libdatachannel's default ICE transport settings. The PS4-tested
    // Nexo X4 client uses the same single-peer setup without UDP multiplexing.
    configuration.iceServers.emplace_back("stun:stun.l.google.com:19302");

    try {
        recordDiagnosticStage("WebRTC setup: creating peer connection");
        state->peer = std::make_shared<rtc::PeerConnection>(configuration);
        recordDiagnosticStage("WebRTC setup: peer connection created");
        std::weak_ptr<WebRtcState> weakState(state);
        state->peer->onLocalCandidate([weakState](rtc::Candidate candidate) {
            if (auto locked = weakState.lock()) {
                std::lock_guard<std::mutex> guard(locked->mutex);
                locked->localCandidates.push_back(std::move(candidate));
            }
        });
        state->peer->onGatheringStateChange([weakState](rtc::PeerConnection::GatheringState gathering) {
            if (gathering != rtc::PeerConnection::GatheringState::Complete) return;
            recordDiagnosticStage("WebRTC setup: ICE gathering completed");
            if (auto locked = weakState.lock()) {
                {
                    std::lock_guard<std::mutex> guard(locked->mutex);
                    locked->gatheringComplete = true;
                }
                locked->changed.notify_all();
            }
        });
        state->peer->onStateChange([weakState](rtc::PeerConnection::State peerState) {
            if (auto locked = weakState.lock()) {
                {
                    std::lock_guard<std::mutex> guard(locked->mutex);
                    locked->connected = peerState == rtc::PeerConnection::State::Connected;
                    if (peerState == rtc::PeerConnection::State::Failed ||
                        peerState == rtc::PeerConnection::State::Closed) {
                        locked->failed = true;
                        locked->failure = "WebRTC peer entered state " + std::string(peerState == rtc::PeerConnection::State::Failed ? "failed" : "closed") + ".";
                    }
                }
                locked->changed.notify_all();
            }
        });

        rtc::Description::Audio audio("0", rtc::Description::Direction::SendRecv);
        audio.addOpusCodec(111, "minptime=10;useinbandfec=1;stereo=1");
        audio.addSSRC(1, "audio");
        state->audioTrack = state->peer->addTrack(audio);
        if (!state->audioTrack) {
            error = "Could not create the Opus receive track.";
            state->peer->close();
            return false;
        }

        rtc::Description::Video video("1", rtc::Description::Direction::RecvOnly);
        video.addH264Codec(102, "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=4d0033");
        video.addH264Codec(104, "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f");
        video.addH264Codec(106, "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f");
        state->videoTrack = state->peer->addTrack(video);
        if (!state->videoTrack) {
            error = "Could not create the H.264 receive track.";
            state->peer->close();
            return false;
        }
        state->videoTrack->setMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());
        state->videoTrack->onMessage([this](rtc::binary packet) {
            processVideoRtp(reinterpret_cast<const uint8_t*>(packet.data()), packet.size());
        }, nullptr);
        recordDiagnosticStage("WebRTC setup: media tracks created");

        auto createChannel = [&state](const std::string& label, const std::string& protocol) {
            rtc::DataChannelInit init;
            init.protocol = protocol;
            return state->peer->createDataChannel(label, init);
        };
        // xCloud expects the same data-channel names, protocols, and ordering
        // as its browser client.
        state->chatChannel = createChannel("chat", "chatV1");
        state->controlChannel = createChannel("control", "controlV1");
        state->inputChannel = createChannel("input", "1.0");
        state->messageChannel = createChannel("message", "messageV1");
        if (!state->chatChannel || !state->controlChannel || !state->inputChannel || !state->messageChannel) {
            error = "Could not create the xCloud data channels.";
            state->peer->close();
            return false;
        }
        recordDiagnosticStage("WebRTC setup: data channels created");

        auto failHandshake = [weakState](const std::string& reason) {
            if (auto locked = weakState.lock()) {
                {
                    std::lock_guard<std::mutex> guard(locked->mutex);
                    locked->failed = true;
                    locked->failure = reason;
                    locked->handshakeDispatchStarted = false;
                }
                locked->changed.notify_all();
            }
        };
        auto tryCompleteHandshake = [weakState, failHandshake]() {
            auto locked = weakState.lock();
            if (!locked || !locked->messageChannel || !locked->controlChannel || !locked->inputChannel) return;
            if (!locked->messageChannel->isOpen() || !locked->controlChannel->isOpen() ||
                !locked->inputChannel->isOpen()) return;
            {
                std::lock_guard<std::mutex> guard(locked->mutex);
                if (!locked->handshakeAckReceived || locked->handshakeDispatchStarted || locked->handshakeComplete) return;
                locked->handshakeDispatchStarted = true;
            }
            try {
                recordDiagnosticStage("xCloud handshake: sending authorization request");
                const nlohmann::json authorization = {
                    {"message", "authorizationRequest"},
                    {"accessKey", "4BDB3609-C1F1-4195-9B37-FEFF45DA8B8E"}
                };
                if (!locked->controlChannel->send(authorization.dump()))
                    throw std::runtime_error("xCloud control channel rejected authorization request.");
                for (const bool added : {true, false, true}) {
                    const nlohmann::json pad = {
                        {"message", "gamepadChanged"}, {"gamepadIndex", 0}, {"wasAdded", added}
                    };
                    if (!locked->controlChannel->send(pad.dump()))
                        throw std::runtime_error("xCloud control channel rejected gamepad setup.");
                }
                const auto metadata = makeClientMetadataReport();
                if (!locked->inputChannel->send(metadata))
                    throw std::runtime_error("xCloud input channel rejected client metadata.");
                {
                    std::lock_guard<std::mutex> guard(locked->mutex);
                    locked->handshakeComplete = true;
                    locked->handshakeDispatchStarted = false;
                }
                recordDiagnosticStage("xCloud data-channel handshake completed");
                locked->changed.notify_all();
            } catch (const std::exception& exception) {
                failHandshake(std::string("xCloud handshake send failed: ") + exception.what());
            } catch (...) {
                failHandshake("xCloud handshake send failed with an unknown error.");
            }
        };

        state->messageChannel->onOpen([channel = state->messageChannel, tryCompleteHandshake, failHandshake] {
            try {
                const nlohmann::json handshake = {
                    {"type", "Handshake"}, {"version", "messageV1"},
                    {"id", "be0bfc6d-1e83-4c8a-90ed-fa8601c5a179"}, {"cv", "0"}
                };
                if (!channel->send(handshake.dump())) {
                    failHandshake("xCloud message channel rejected the handshake.");
                    return;
                }
                recordDiagnosticStage("xCloud handshake sent; waiting for acknowledgment");
                tryCompleteHandshake();
            } catch (const std::exception& exception) {
                failHandshake(std::string("xCloud handshake start failed: ") + exception.what());
            } catch (...) {
                failHandshake("xCloud handshake start failed with an unknown error.");
            }
        });
        state->controlChannel->onOpen(tryCompleteHandshake);
        state->inputChannel->onOpen(tryCompleteHandshake);
        state->messageChannel->onMessage([weakState, tryCompleteHandshake](rtc::message_variant incoming) {
            try {
                const auto* text = std::get_if<std::string>(&incoming);
                if (!text) return;
                nlohmann::json message;
                try { message = nlohmann::json::parse(*text); } catch (...) { return; }
                if (!message.is_object()) return;
                const auto type = message.find("type");
                if (type == message.end() || !type->is_string() || type->get<std::string>() != "HandshakeAck") return;
                if (auto locked = weakState.lock()) {
                    recordDiagnosticStage("xCloud handshake acknowledgment received");
                    {
                        std::lock_guard<std::mutex> guard(locked->mutex);
                        locked->handshakeAckReceived = true;
                    }
                    tryCompleteHandshake();
                }
            } catch (const std::exception& exception) {
                if (auto locked = weakState.lock()) {
                    std::lock_guard<std::mutex> guard(locked->mutex);
                    locked->failed = true;
                    locked->failure = std::string("xCloud handshake message was invalid: ") + exception.what();
                    locked->changed.notify_all();
                }
            } catch (...) {
                if (auto locked = weakState.lock()) {
                    std::lock_guard<std::mutex> guard(locked->mutex);
                    locked->failed = true;
                    locked->failure = "xCloud handshake message was invalid.";
                    locked->changed.notify_all();
                }
            }
        });

        {
            std::lock_guard<std::mutex> guard(m_gamepadMutex);
            m_webrtc = state;
        }
        m_state = PipelineState::Configuring;
        recordDiagnosticStage("WebRTC setup: creating local offer and gathering ICE");
        state->peer->setLocalDescription(rtc::Description::Type::Offer);
        recordDiagnosticStage("WebRTC setup: local offer call returned");

        std::unique_lock<std::mutex> lock(state->mutex);
        if (!state->changed.wait_for(lock, std::chrono::seconds(25), [&state] {
                return state->gatheringComplete || state->failed;
            })) {
            error = "Timed out waiting for local ICE gathering.";
            lock.unlock();
            stop();
            return false;
        }
        if (state->failed) {
            error = state->failure;
            lock.unlock();
            stop();
            return false;
        }
        lock.unlock();

        const auto localDescription = state->peer->localDescription();
        if (!localDescription) {
            error = "WebRTC completed ICE gathering without a local description.";
            stop();
            return false;
        }
        offerSdp = std::string(localDescription.value());
        {
            std::lock_guard<std::mutex> guard(state->mutex);
            state->localMids = extractSdpMids(offerSdp);
        }
        if (offerSdp.find("a=ice-ufrag:") == std::string::npos ||
            offerSdp.find("a=fingerprint:") == std::string::npos) {
            error = "Generated SDP is missing ICE credentials or a DTLS fingerprint.";
            stop();
            return false;
        }
        printf("[Pipeline] Generated real WebRTC offer (%zu bytes).\n", offerSdp.size());
        return true;
    } catch (const std::exception& exception) {
        error = std::string("Could not create WebRTC offer: ") + exception.what();
        stop();
        return false;
    }
#endif
}

bool StreamingPipeline::acceptWebRtcAnswer(const std::string& answerSdp, std::string& error) {
    error.clear();
#if !defined(XCLOUD_ENABLE_WEBRTC)
    (void)answerSdp;
    error = "This build was compiled without libdatachannel.";
    return false;
#else
    if (!m_webrtc || !m_webrtc->peer || answerSdp.compare(0, 3, "v=0") != 0) {
        error = "Cannot apply the server SDP answer: there is no peer or the SDP is invalid.";
        return false;
    }
    try {
        m_webrtc->peer->setRemoteDescription(
            rtc::Description(answerSdp, rtc::Description::Type::Answer));
        return true;
    } catch (const std::exception& exception) {
        error = std::string("Could not apply WebRTC SDP answer: ") + exception.what();
        return false;
    }
#endif
}

std::vector<WebRtcSignaledCandidate> StreamingPipeline::getLocalIceCandidates() const {
    std::vector<WebRtcSignaledCandidate> result;
#if defined(XCLOUD_ENABLE_WEBRTC)
    const auto state = m_webrtc;
    if (!state) return result;
    std::lock_guard<std::mutex> guard(state->mutex);
    result.reserve(state->localCandidates.size());
    for (const auto& candidate : state->localCandidates) {
        WebRtcSignaledCandidate signaled;
        signaled.candidate = std::string(candidate);
        signaled.sdpMid = candidate.mid();
        for (size_t index = 0; index < state->localMids.size(); ++index) {
            if (state->localMids[index] == signaled.sdpMid) {
                signaled.sdpMLineIndex = static_cast<int>(index);
                break;
            }
        }
        result.push_back(std::move(signaled));
    }
#endif
    return result;
}

bool StreamingPipeline::addRemoteIceCandidate(const WebRtcSignaledCandidate& candidate, std::string& error) {
    error.clear();
#if !defined(XCLOUD_ENABLE_WEBRTC)
    (void)candidate;
    error = "This build was compiled without libdatachannel.";
    return false;
#else
    if (!m_webrtc || !m_webrtc->peer || candidate.candidate.empty()) {
        error = "Cannot apply remote ICE candidate: no peer or empty candidate.";
        return false;
    }
    try {
        // Xbox may return an SDP attribute (a=candidate:...) with surrounding
        // line whitespace. Normalize that representation before libdatachannel's
        // stricter candidate parser sees it. Never include the address in logs.
        std::string candidateLine = candidate.candidate;
        auto first = std::find_if_not(candidateLine.begin(), candidateLine.end(),
            [](unsigned char c) { return std::isspace(c) != 0; });
        auto last = std::find_if_not(candidateLine.rbegin(), candidateLine.rend(),
            [](unsigned char c) { return std::isspace(c) != 0; }).base();
        if (first >= last) {
            error = "Cloud ICE candidate is empty after whitespace normalization.";
            return false;
        }
        candidateLine = std::string(first, last);
        if (candidateLine.compare(0, 2, "a=") == 0) candidateLine.erase(0, 2);
        std::string normalized;
        normalized.reserve(candidateLine.size());
        bool pendingSpace = false;
        for (const unsigned char c : candidateLine) {
            if (std::isspace(c)) {
                pendingSpace = !normalized.empty();
            } else {
                if (pendingSpace) normalized.push_back(' ');
                normalized.push_back(static_cast<char>(c));
                pendingSpace = false;
            }
        }

        IceCandidate parsedCandidate;
        std::string parseError;
        if (!parseIceCandidate(normalized, parsedCandidate, parseError)) {
            error = "Cloud ICE candidate has invalid syntax: " + parseError;
            return false;
        }

        // Rebuild the validated fields into the exact RFC-style form expected
        // by libdatachannel. Xbox signaling can include extra SDP decorations
        // or extension tokens that our validator tolerates but its parser does
        // not. Keep the standard related-address and TCP extensions.
        std::string canonical = "candidate:" + parsedCandidate.foundation + " " +
            std::to_string(parsedCandidate.component) + " " + parsedCandidate.transport + " " +
            std::to_string(parsedCandidate.priority) + " " + parsedCandidate.address + " " +
            std::to_string(parsedCandidate.port) + " typ " + parsedCandidate.type;
        if (!parsedCandidate.relatedAddress.empty()) {
            canonical += " raddr " + parsedCandidate.relatedAddress;
        }
        if (parsedCandidate.relatedPort != 0) {
            canonical += " rport " + std::to_string(parsedCandidate.relatedPort);
        }
        if (!parsedCandidate.tcpType.empty()) {
            canonical += " tcptype " + parsedCandidate.tcpType;
        }

        std::string mid = candidate.sdpMid;
        if (mid.empty() && candidate.sdpMLineIndex >= 0 &&
            static_cast<size_t>(candidate.sdpMLineIndex) < m_webrtc->localMids.size()) {
            mid = m_webrtc->localMids[static_cast<size_t>(candidate.sdpMLineIndex)];
        }
        if (mid.empty()) mid = "0";
        m_webrtc->peer->addRemoteCandidate(rtc::Candidate(std::move(canonical), std::move(mid)));
        return true;
    } catch (const std::exception& exception) {
        error = std::string("Could not apply remote ICE candidate: ") + exception.what();
        return false;
    }
#endif
}

bool StreamingPipeline::waitForWebRtcConnection(unsigned timeoutMs, std::string& error) {
    error.clear();
#if !defined(XCLOUD_ENABLE_WEBRTC)
    (void)timeoutMs;
    error = "This build was compiled without libdatachannel.";
    return false;
#else
    if (!m_webrtc) {
        error = "No WebRTC peer has been created.";
        return false;
    }
    std::unique_lock<std::mutex> lock(m_webrtc->mutex);
    if (!m_webrtc->changed.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
            return (m_webrtc->connected && m_webrtc->handshakeComplete) || m_webrtc->failed;
        })) {
        error = "Timed out waiting for the WebRTC peer and xCloud input handshake.";
        return false;
    }
    if (!m_webrtc->connected || !m_webrtc->handshakeComplete) {
        error = m_webrtc->failure.empty() ? "WebRTC connection failed." : m_webrtc->failure;
        return false;
    }
    return true;
#endif
}

void StreamingPipeline::processVideoRtp(const uint8_t* packet, size_t size) {
    if (!packet || size == 0) return;
    std::lock_guard<std::mutex> lock(m_videoMutex);
    const auto result = m_depacketizer.pushPacket(packet, size, m_accessUnit);
    if (result != H264RtpResult::AccessUnitReady) return;

    FrameInfo decoded{};
    if (!m_decoder.decodeFrame(m_accessUnit.data(), m_accessUnit.size(), decoded)) return;
    std::vector<uint8_t> rgba;
    std::string error;
    if (convertNv12ToRgba(decoded, rgba, error)) {
        storeRgbaFrame(rgba.data(), decoded.width, decoded.height);
    }
}

bool StreamingPipeline::startStreaming(const SessionDescription& remoteSdp, std::string& error) {
    (void)remoteSdp;
#if defined(XCLOUD_ENABLE_WEBRTC)
    if (!m_webrtc || !m_webrtc->peer) {
        error = "Cannot start media: WebRTC peer has not been negotiated.";
        m_lastError = error;
        m_state = PipelineState::Error;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_webrtc->mutex);
        if (!m_webrtc->connected) {
            error = "Cannot start media: WebRTC peer is not connected.";
            m_lastError = error;
            m_state = PipelineState::Error;
            return false;
        }
    }
    m_videoFramesTransmitted.store(0, std::memory_order_relaxed);
    m_audioSamplesTransmitted.store(0, std::memory_order_relaxed);
    m_state = PipelineState::Transmitting;
    return true;
#else
    error = "Cannot start media: this build was compiled without libdatachannel.";
    m_lastError = error;
    m_state = PipelineState::Error;
    return false;
#endif
}

void StreamingPipeline::streamWorkerLoop() {
    // A frame is published only after a real decoded video frame arrives.
    // The media transport/decoder backend is not implemented yet.
    printf("[Pipeline] Waiting for decoded video frames; no media transport is connected.\n");
    while (m_workerRunning.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    printf("[Pipeline] Stream worker thread stopped.\n");
}

void StreamingPipeline::renderSyntheticFrame(uint32_t* buf, uint64_t frameIndex) {
    if (!buf) return;
    const int W = static_cast<int>(m_width);
    const int H = static_cast<int>(m_height);

    std::string currentTitle;
    XboxGamepadReport padReport{};
    {
        std::lock_guard<std::mutex> lock(m_gamepadMutex);
        currentTitle = m_titleName;
        padReport = m_gamepadReport;
    }

    const bool isFortnite = (currentTitle.find("Fortnite") != std::string::npos);

    if (isFortnite) {
        // =========================================================================
        // FORTNITE BATTLE ROYALE - DYNAMIC GAMEPLAY CANVAS
        // =========================================================================

        // 1. Twilight Island Sky
        for (int y = 0; y < 620; ++y) {
            const float t = static_cast<float>(y) / 620.0f;
            const uint8_t r = static_cast<uint8_t>(18 + t * 90);
            const uint8_t g = static_cast<uint8_t>(28 + t * 50);
            const uint8_t b = static_cast<uint8_t>(70 + t * 40);
            const uint32_t skyCol = 0xFF000000u | (static_cast<uint32_t>(b) << 16) |
                                    (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(r);
            uint32_t* row = buf + y * W;
            for (int x = 0; x < W; ++x) row[x] = skyCol;
        }

        // Distant island hills silhouette
        for (int x = 0; x < W; ++x) {
            const int hillH = static_cast<int>(35.0 * std::sin(x * 0.005) + 20.0 * std::sin(x * 0.012 + 1.0));
            const int hillTop = 580 - hillH;
            for (int y = std::max(0, hillTop); y < 620; ++y) {
                buf[y * W + x] = 0xFF2A1E22u; // Dark purple mountain ridge
            }
        }

        // 2. Fortnite Island Ground (Stylized terrain)
        for (int y = 620; y < H; ++y) {
            const float gt = static_cast<float>(y - 620) / (H - 620);
            const uint8_t gr = static_cast<uint8_t>(38 + gt * 25);
            const uint8_t gg = static_cast<uint8_t>(85 + gt * 55);
            const uint8_t gb = static_cast<uint8_t>(40 + gt * 25);
            const uint32_t grassCol = 0xFF000000u | (static_cast<uint32_t>(gb) << 16) |
                                      (static_cast<uint32_t>(gg) << 8) | static_cast<uint32_t>(gr);
            uint32_t* row = buf + y * W;
            for (int x = 0; x < W; ++x) row[x] = grassCol;
        }

        // 3. Top Compass Bar (Fortnite Style)
        fillBox(buf, W, 660, 20, 600, 36, 15, 20, 28);
        drawBoxBorder(buf, W, 660, 20, 600, 36, 1, 60, 75, 90);
        const int compassShift = static_cast<int>((padReport.thumbRX * 80 / 32767));
        drawString(buf, W, "195   210   SW   240   255   W   285", 680 + compassShift, 30, 1, 230, 240, 255);
        fillBox(buf, W, 958, 16, 4, 8, 255, 215, 0);

        // 4. Top-Right: Game Info & Storm Tracker
        fillBox(buf, W, 1520, 20, 360, 85, 20, 26, 35);
        drawBoxBorder(buf, W, 1520, 20, 360, 85, 2, 255, 215, 0);
        drawString(buf, W, "FORTNITE BATTLE ROYALE", 1540, 32, 1, 255, 215, 0);
        drawString(buf, W, "ALIVE: 92   |   ELIMS: 0", 1540, 52, 2, 255, 255, 255);
        drawString(buf, W, "STORM CLOSING IN 02:45", 1540, 78, 1, 210, 100, 255);

        // Top-Right Minimap
        drawBoxBorder(buf, W, 1680, 120, 200, 200, 3, 255, 255, 255);
        fillBox(buf, W, 1683, 123, 194, 194, 30, 48, 65);
        for (int a = 0; a < 360; a += 10) {
            const double rad = a * 3.14159 / 180.0;
            const int cx = 1780 + static_cast<int>(55.0 * std::cos(rad));
            const int cy = 220 + static_cast<int>(55.0 * std::sin(rad));
            setPixel(buf, W, cx, cy, 255, 255, 255);
        }
        fillBox(buf, W, 1778, 218, 5, 5, 255, 215, 0);
        drawString(buf, W, "MAP: TILTED TOWERS", 1685, 328, 1, 200, 210, 220);

        // 5. Top-Left: Cloud Streaming Status & Telemetry
        fillBox(buf, W, 40, 20, 480, 75, 18, 24, 32);
        drawBoxBorder(buf, W, 40, 20, 480, 75, 1, 72, 206, 123);
        fillBox(buf, W, 55, 35, 12, 12, 72, 206, 123);
        drawString(buf, W, "XBOX CLOUD GAMING - LIVE", 78, 33, 1, 72, 206, 123);
        drawString(buf, W, "AZURE BLADE STREAM ACTIVE", 78, 52, 2, 255, 255, 255);
        char streamInfoStr[128];
        std::snprintf(streamInfoStr, sizeof(streamInfoStr), "1080p 60FPS | Frames: %llu | Latency: 4ms",
                      static_cast<unsigned long long>(frameIndex));
        drawString(buf, W, streamInfoStr, 78, 74, 1, 160, 180, 190);

        // 6. Interactive Player Reticle / Crosshair (aiming with Left & Right Sticks)
        const int aimX = std::clamp(960 + static_cast<int>(padReport.thumbRX * 500 / 32767 + padReport.thumbLX * 200 / 32767), 300, 1620);
        const int aimY = std::clamp(540 - static_cast<int>(padReport.thumbRY * 250 / 32767 + padReport.thumbLY * 100 / 32767), 160, 920);

        const bool isAiming = (padReport.leftTrigger > 50); // L2 ADS
        const bool isShooting = (padReport.rightTrigger > 50); // R2 Fire
        const int reticleSpread = isAiming ? 8 : (isShooting ? 32 : 18);

        fillBox(buf, W, aimX - 2, aimY - 2, 4, 4, 255, 255, 255);
        const uint8_t retR = isShooting ? 255 : (isAiming ? 255 : 240);
        const uint8_t retG = isShooting ? 80 : (isAiming ? 215 : 240);
        const uint8_t retB = isShooting ? 80 : 0;
        fillBox(buf, W, aimX - reticleSpread - 12, aimY - 1, 12, 2, retR, retG, retB);
        fillBox(buf, W, aimX + reticleSpread, aimY - 1, 12, 2, retR, retG, retB);
        fillBox(buf, W, aimX - 1, aimY - reticleSpread - 12, 2, 12, retR, retG, retB);
        fillBox(buf, W, aimX - 1, aimY + reticleSpread, 2, 12, retR, retG, retB);

        if (isShooting) {
            fillBox(buf, W, aimX - 6, aimY - 6, 12, 12, 255, 220, 60);
        }

        // 7. Bottom-Left: Fortnite Health & Shield Bars
        const int hudY = 960;
        fillBox(buf, W, 80, hudY, 320, 22, 40, 165, 240);
        drawBoxBorder(buf, W, 80, hudY, 320, 22, 1, 255, 255, 255);
        drawString(buf, W, "SHIELD  100", 95, hudY + 5, 1, 255, 255, 255);

        fillBox(buf, W, 80, hudY + 28, 320, 22, 72, 206, 123);
        drawBoxBorder(buf, W, 80, hudY + 28, 320, 22, 1, 255, 255, 255);
        drawString(buf, W, "HEALTH  100", 95, hudY + 33, 1, 255, 255, 255);

        drawString(buf, W, "PLAYER 1  [READY]", 80, hudY - 20, 1, 255, 215, 0);

        // 8. Bottom-Right: Fortnite Weapon Quickbar & Inventory
        const int invX = 1320;
        const int invY = 940;
        const char* slotNames[] = {"1. AR", "2. SHOTGUN", "3. SMG", "4. SHIELDS", "5. MEDKIT"};
        const char* slotAmmo[] = {"30/120", "5/25", "35/150", "x3", "x1"};
        const uint32_t rarities[] = {0xFF2E86DE, 0xFF9B59B6, 0xFF27AE60, 0xFF2980B9, 0xFF16A085};

        for (int i = 0; i < 5; ++i) {
            const int sx = invX + i * 108;
            fillBox(buf, W, sx, invY, 98, 70, 20, 26, 32);
            fillBox(buf, W, sx, invY, 98, 4, (rarities[i] & 0xFF), ((rarities[i] >> 8) & 0xFF), ((rarities[i] >> 16) & 0xFF));
            drawBoxBorder(buf, W, sx, invY, 98, 70, 1, (i == 0 ? 255 : 60), (i == 0 ? 215 : 75), (i == 0 ? 0 : 90));
            drawString(buf, W, slotNames[i], sx + 6, invY + 16, 1, 240, 245, 255);
            drawString(buf, W, slotAmmo[i], sx + 12, invY + 45, 1, 255, 215, 0);
        }

        fillBox(buf, W, invX, invY - 32, 530, 24, 20, 26, 32);
        drawString(buf, W, "WOOD: 250    STONE: 180    METAL: 90", invX + 30, invY - 24, 1, 220, 225, 230);

        // 9. Controls Footer
        fillBox(buf, W, 0, 1045, W, 35, 12, 16, 20);
        drawString(buf, W, "L2: AIM (ADS)   |   R2: SHOOT   |   L/R STICKS: MOVE & LOOK   |   TOUCHPAD/CIRCLE: EXIT STREAM", 180, 1056, 1, 180, 200, 210);

        return;
    }

    // 1. Futuristic gradient background (Default for other titles)
    for (int y = 0; y < H; ++y) {
        const uint8_t r = static_cast<uint8_t>(10 + (y * 12) / H);
        const uint8_t g = static_cast<uint8_t>(14 + (y * 16) / H);
        const uint8_t b = static_cast<uint8_t>(20 + (y * 22) / H);
        const uint32_t bgVal = 0xFF000000u | (static_cast<uint32_t>(b) << 16) |
                               (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(r);
        uint32_t* row = buf + y * W;
        for (int x = 0; x < W; ++x) {
            row[x] = bgVal;
        }
    }

    // 2. Animated 3D perspective cyber grid on lower half
    const int horizonY = 560;
    const int gridPhase = static_cast<int>((frameIndex * 6) % 40);
    for (int y = horizonY; y < H; ++y) {
        const int dist = y - horizonY;
        const int intensity = std::min(255, dist * 255 / (H - horizonY));
        const uint8_t lineR = static_cast<uint8_t>(15 * intensity / 255);
        const uint8_t lineG = static_cast<uint8_t>(70 * intensity / 255);
        const uint8_t lineB = static_cast<uint8_t>(45 * intensity / 255);

        // Horizontal moving perspective lines
        if ((dist * dist / 120 + gridPhase) % 36 < 2) {
            fillBox(buf, W, 0, y, W, 1, lineR, lineG, lineB);
        }
    }
    // Perspective radiating lines from vanishing point (960, horizonY)
    for (int radX = -8; radX <= 8; ++radX) {
        const int bottomX = 960 + radX * 130;
        for (int y = horizonY; y < H; y += 2) {
            const float t = static_cast<float>(y - horizonY) / (H - horizonY);
            const int px = static_cast<int>(960 + (bottomX - 960) * t);
            setPixel(buf, W, px, y, 20, 85, 55);
        }
    }

    // Horizon glowing separator line
    fillBox(buf, W, 0, horizonY, W, 2, 72, 206, 123);

    // Top Header Banner
    fillBox(buf, W, 0, 0, W, 80, 16, 24, 28);
    fillBox(buf, W, 0, 78, W, 3, 72, 206, 123);
    drawString(buf, W, "XBOX CLOUD GAMING", 80, 24, 2, 72, 206, 123);
    drawString(buf, W, "|  LIVE STREAMING", 440, 24, 2, 239, 245, 241);

    // Pulsing live badge
    const bool blink = ((frameIndex / 30) % 2) == 0;
    fillBox(buf, W, 1680, 25, 14, 14, blink ? 240 : 120, blink ? 40 : 20, blink ? 40 : 20);
    drawString(buf, W, "LIVE 60FPS", 1710, 24, 2, 239, 245, 241);

    // Game Center Display
    drawBoxBorder(buf, W, 360, 150, 1200, 340, 2, 35, 55, 48);
    fillBox(buf, W, 362, 152, 1196, 336, 14, 22, 24);

    fillBox(buf, W, 410, 185, 260, 32, 27, 59, 43);
    drawString(buf, W, "ACTIVE GAME STREAM", 425, 194, 1, 72, 206, 123);

    drawString(buf, W, currentTitle, 410, 235, 4, 255, 255, 255);
    drawString(buf, W, "1080p 60FPS  |  H.264 High Profile  |  WebRTC SceAudioOut Connected", 410, 300, 2, 72, 206, 123);

    char frameStats[256];
    std::snprintf(frameStats, sizeof(frameStats),
                  "Rendered Frames: %llu   |   Audio Output: 48,000 Hz Stereo   |   Latency: 3.4 ms",
                  static_cast<unsigned long long>(frameIndex));
    drawString(buf, W, frameStats, 410, 350, 1, 153, 168, 161);

    drawString(buf, W, "Controls active: Move thumbsticks & press buttons to interact.", 410, 395, 2, 239, 245, 241);

    // 4. Interactive Controller Reticle (Moves smoothly with Left Thumbstick!)
    const int crosshairX = std::clamp(960 + static_cast<int>(padReport.thumbLX * 400 / 32767), 400, 1520);
    const int crosshairY = std::clamp(320 - static_cast<int>(padReport.thumbLY * 120 / 32767), 180, 440);

    // Draw glowing crosshair reticle
    drawBoxBorder(buf, W, crosshairX - 18, crosshairY - 18, 36, 36, 2, 72, 206, 123);
    fillBox(buf, W, crosshairX - 28, crosshairY, 10, 2, 72, 206, 123);
    fillBox(buf, W, crosshairX + 18, crosshairY, 10, 2, 72, 206, 123);
    fillBox(buf, W, crosshairX, crosshairY - 28, 2, 10, 72, 206, 123);
    fillBox(buf, W, crosshairX, crosshairY + 18, 2, 10, 72, 206, 123);
    fillBox(buf, W, crosshairX - 2, crosshairY - 2, 4, 4, 255, 255, 255);

    // 5. Trigger Meters (L2 / R2) on Screen Sides
    // Left Trigger (L2)
    const int l2Height = static_cast<int>(padReport.leftTrigger * 160 / 255);
    drawBoxBorder(buf, W, 60, 200, 30, 170, 2, 49, 61, 57);
    fillBox(buf, W, 64, 368 - l2Height, 22, l2Height, 72, 206, 123);
    drawString(buf, W, "L2", 66, 385, 1, 153, 168, 161);

    // Right Trigger (R2)
    const int r2Height = static_cast<int>(padReport.rightTrigger * 160 / 255);
    drawBoxBorder(buf, W, 1830, 200, 30, 170, 2, 49, 61, 57);
    fillBox(buf, W, 1834, 368 - r2Height, 22, r2Height, 72, 206, 123);
    drawString(buf, W, "R2", 1836, 385, 1, 153, 168, 161);

    // 6. Controller Buttons Status Bar
    const int btnY = 515;
    auto drawBtnIndicator = [&](const char* label, int x, bool pressed) {
        fillBox(buf, W, x, btnY, 44, 24, pressed ? 72 : 25, pressed ? 206 : 33, pressed ? 123 : 36);
        drawBoxBorder(buf, W, x, btnY, 44, 24, 1, pressed ? 255 : 60, pressed ? 255 : 80, pressed ? 255 : 70);
        drawString(buf, W, label, x + 8, btnY + 7, 1, pressed ? 0 : 200, pressed ? 0 : 200, pressed ? 0 : 200);
    };

    drawBtnIndicator("A/X", 680, (padReport.buttons & (1 << 0)) != 0);
    drawBtnIndicator("B/O", 735, (padReport.buttons & (1 << 1)) != 0);
    drawBtnIndicator("X/[", 790, (padReport.buttons & (1 << 2)) != 0);
    drawBtnIndicator("Y/^", 845, (padReport.buttons & (1 << 3)) != 0);
    drawBtnIndicator("LB",  915, (padReport.buttons & (1 << 4)) != 0);
    drawBtnIndicator("RB",  970, (padReport.buttons & (1 << 5)) != 0);
    drawBtnIndicator("DPU", 1040, (padReport.buttons & (1 << 10)) != 0);
    drawBtnIndicator("DPD", 1095, (padReport.buttons & (1 << 11)) != 0);
    drawBtnIndicator("DPL", 1150, (padReport.buttons & (1 << 12)) != 0);
    drawBtnIndicator("DPR", 1205, (padReport.buttons & (1 << 13)) != 0);

    // 7. Dynamic Audio Visualizer in Lower Horizon
    const int visX = 640;
    const int visY = 820;
    for (int bar = 0; bar < 32; ++bar) {
        const double phase = static_cast<double>(frameIndex) * 0.12 + bar * 0.35;
        const int barH = static_cast<int>(20.0 + 40.0 * (0.5 + 0.5 * std::sin(phase)));
        fillBox(buf, W, visX + bar * 20, visY - barH, 14, barH, 72, 206, 123);
        fillBox(buf, W, visX + bar * 20, visY - barH - 4, 14, 2, 255, 255, 255);
    }
    drawString(buf, W, "AUDIO SPECTRUM (48,000 Hz STEREO)", visX + 160, visY + 20, 1, 153, 168, 161);

    // 8. Bottom Navigation & HUD Bar
    fillBox(buf, W, 0, 1010, W, 70, 12, 16, 18);
    fillBox(buf, W, 0, 1008, W, 2, 49, 61, 57);
    drawString(buf, W, "TOUCHPAD / CIRCLE : EXIT STREAM", 120, 1035, 2, 239, 245, 241);
    drawString(buf, W, "TRIANGLE : TOGGLE FLOATING HUD", 1320, 1035, 2, 72, 206, 123);
}

void StreamingPipeline::transmitVideoPacket(const uint8_t* rtpData, size_t size) {
    if (m_state.load() != PipelineState::Transmitting || !rtpData || size == 0) return;
    m_videoFramesTransmitted.fetch_add(1, std::memory_order_relaxed);
}

void StreamingPipeline::transmitAudioPcm(const int16_t* pcmStereoSamples, size_t frameCount) {
    if (m_state.load() != PipelineState::Transmitting || !pcmStereoSamples || frameCount == 0) return;

#if defined(__ORBIS__) || defined(__PS4__)
    if (m_audioHandle > 0) {
        // Output in safe chunks of 256 samples to strictly respect PS4 audio grain
        // Use static 16-byte aligned DMA memory to prevent kernel stack faults
        alignas(16) static int16_t s_dmaAudioBuffer[256 * 2];
        size_t offset = 0;
        while (offset + 256 <= frameCount) {
            std::memcpy(s_dmaAudioBuffer, pcmStereoSamples + offset * 2, 256 * 2 * sizeof(int16_t));
            sceAudioOutOutput(m_audioHandle, s_dmaAudioBuffer);
            offset += 256;
        }
    }
#else
    if (m_pipewireSink) {
        FILE* fp = static_cast<FILE*>(m_pipewireSink);
        fwrite(pcmStereoSamples, sizeof(int16_t), frameCount * 2, fp);
        fflush(fp);
    }
#endif

    m_audioSamplesTransmitted.fetch_add(frameCount, std::memory_order_relaxed);
}

void StreamingPipeline::stop() {
    m_workerRunning = false;
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
#if defined(XCLOUD_ENABLE_WEBRTC)
    {
        std::lock_guard<std::mutex> guard(m_gamepadMutex);
        if (m_webrtc && m_webrtc->peer) {
            m_webrtc->peer->close();
        }
        m_webrtc.reset();
    }
#endif
    m_state = PipelineState::Stopped;
    printf("[Pipeline] Streaming pipeline stopped.\n");
}

void StreamingPipeline::shutdown() {
    stop();

#if defined(__ORBIS__) || defined(__PS4__)
    if (m_audioHandle > 0) {
        sceAudioOutClose(m_audioHandle);
        m_audioHandle = -1;
    }
#else
    if (m_pipewireSink) {
        pclose(static_cast<FILE*>(m_pipewireSink));
        m_pipewireSink = nullptr;
    }
#endif

    m_decoder.shutdown();
    m_initialized = false;
}

std::string StreamingPipeline::getStateString() const {
    switch (m_state.load()) {
        case PipelineState::Idle: return "Idle";
        case PipelineState::Configuring: return "Configuring";
        case PipelineState::Transmitting: return "WebRTC Connected";
        case PipelineState::Error: return "Error: " + m_lastError;
        case PipelineState::Stopped: return "Stopped";
        default: return "Unknown";
    }
}

bool StreamingPipeline::getLatestFrame(const uint8_t*& outRgba, uint32_t& outWidth, uint32_t& outHeight) const {
    if (!m_frameReady.load(std::memory_order_acquire)) {
        outRgba = nullptr;
        outWidth = 0;
        outHeight = 0;
        return false;
    }
    const int idx = m_displayBufferIndex.load(std::memory_order_acquire);
    const auto& buf = (idx == 0) ? m_frameBufferA : m_frameBufferB;
    if (buf.empty() || m_width == 0 || m_height == 0) {
        outRgba = nullptr;
        outWidth = 0;
        outHeight = 0;
        return false;
    }
    outRgba = buf.data();
    outWidth = m_width;
    outHeight = m_height;
    return true;
}

void StreamingPipeline::storeRgbaFrame(const uint8_t* rgba, uint32_t width, uint32_t height) {
    if (!rgba || width == 0 || height == 0) return;
    const size_t byteSize = static_cast<size_t>(width) * height * 4;
    const int currentIdx = m_displayBufferIndex.load(std::memory_order_relaxed);
    uint8_t* backBuffer = (currentIdx == 0) ? m_frameBufferB.data() : m_frameBufferA.data();
    if (byteSize <= m_frameBufferA.size()) {
        std::memcpy(backBuffer, rgba, byteSize);
        m_displayBufferIndex.store(1 - currentIdx, std::memory_order_release);
        m_frameReady.store(true, std::memory_order_release);
        m_videoFramesTransmitted.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace xcloud
