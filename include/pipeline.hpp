#pragma once

#include <string>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <vector>
#include <mutex>
#include <thread>
#include <memory>
#include "webrtc_sdp.hpp"
#include "pad_input.hpp"
#include "h264_rtp.hpp"
#include "video_decoder.hpp"

namespace xcloud {

enum class PipelineState {
    Idle,
    Configuring,
    Transmitting,
    Error,
    Stopped
};

class StreamingPipeline {
public:
    StreamingPipeline();
    ~StreamingPipeline();

    bool init(uint32_t width = 1920, uint32_t height = 1080);
    bool createWebRtcOffer(std::string& offerSdp, std::string& error);
    bool acceptWebRtcAnswer(const std::string& answerSdp, std::string& error);
    std::vector<WebRtcSignaledCandidate> getLocalIceCandidates() const;
    bool addRemoteIceCandidate(const WebRtcSignaledCandidate& candidate, std::string& error);
    bool waitForWebRtcConnection(unsigned timeoutMs, std::string& error);
    bool startStreaming(const SessionDescription& remoteSdp, std::string& error);
    void setStreamInfo(const std::string& titleName, const std::string& remoteIp = "", uint16_t remotePort = 0);
    void updateGamepad(const XboxGamepadReport& report);
    void transmitVideoPacket(const uint8_t* rtpData, size_t size);
    void transmitAudioPcm(const int16_t* pcmStereoSamples, size_t frameCount);
    void stop();
    void shutdown();

    bool getLatestFrame(const uint8_t*& outRgba, uint32_t& outWidth, uint32_t& outHeight) const;
    void storeRgbaFrame(const uint8_t* rgba, uint32_t width, uint32_t height);

    PipelineState getState() const { return m_state.load(); }
    std::string getStateString() const;
    std::string getLastError() const { return m_lastError; }
    bool isTransmitting() const { return m_state.load() == PipelineState::Transmitting; }

    uint64_t getFramesTransmitted() const { return m_videoFramesTransmitted.load(); }
    uint64_t getAudioSamplesTransmitted() const { return m_audioSamplesTransmitted.load(); }

private:
    void streamWorkerLoop();
    void renderSyntheticFrame(uint32_t* dst, uint64_t frameIndex);
    void processVideoRtp(const uint8_t* packet, size_t size);

    struct WebRtcState;
    std::shared_ptr<WebRtcState> m_webrtc;
    std::mutex m_videoMutex;

    std::atomic<PipelineState> m_state{PipelineState::Idle};
    std::string m_lastError;
    int32_t m_audioHandle = -1;
    void* m_pipewireSink = nullptr; // Host PipeWire / pw-cat pipe
    uint32_t m_width = 1920;
    uint32_t m_height = 1080;
    bool m_initialized = false;
    std::atomic<uint64_t> m_videoFramesTransmitted{0};
    std::atomic<uint64_t> m_audioSamplesTransmitted{0};

    // Pre-allocated static double buffers for zero-copy thread-safe flipping
    std::vector<uint8_t> m_frameBufferA;
    std::vector<uint8_t> m_frameBufferB;
    std::atomic<bool> m_frameReady{false};
    std::atomic<int> m_displayBufferIndex{0};

    std::thread m_workerThread;
    std::atomic<bool> m_workerRunning{false};

    // Gamepad state for interactive feedback
    std::mutex m_gamepadMutex;
    XboxGamepadReport m_gamepadReport{};
    std::string m_titleName{"Xbox Cloud Gaming"};
    std::string m_remoteIp;
    uint16_t m_remotePort = 0;

    // Media depacketizer & decoder
    H264RtpDepacketizer m_depacketizer;
    PS4AvcDecoder m_decoder;
    std::vector<uint8_t> m_accessUnit;
};

} // namespace xcloud
