#pragma once
#include <cstdint>
#include <cstddef>

namespace xcloud {

struct FrameInfo {
    uint32_t width;
    uint32_t height;
    uint32_t pitchY;
    uint32_t pitchUV;
    const uint8_t* planeY;
    const uint8_t* planeUV;
};

class PS4AvcDecoder {
public:
    PS4AvcDecoder();
    ~PS4AvcDecoder();

    // Initializes hardware H.264 decoder (libSceAvcdec)
    bool init(uint32_t width = 1920, uint32_t height = 1080);

    // Feeds an H.264 Access Unit (NAL units from WebRTC RTP stream)
    bool decodeFrame(const uint8_t* h264Data, size_t size, FrameInfo& outFrame);

    void shutdown();

private:
    void* m_decoderHandle = nullptr;
    void* m_decoderMemory = nullptr;
    size_t m_memorySize = 0;
    bool m_initialized = false;
};

} // namespace xcloud
