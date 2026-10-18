#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

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
    const std::string& getLastError() const { return m_lastError; }

private:
    void* m_decoderHandle = nullptr;
    void* m_decoderMemory = nullptr;
    size_t m_memorySize = 0;
    void* m_sharedMemory = nullptr;
    size_t m_sharedMemorySize = 0;
    void* m_frameBuffer = nullptr;
    size_t m_frameBufferSize = 0;
    int64_t m_sharedMemoryOffset = -1;
    int64_t m_frameBufferOffset = -1;
    uint32_t m_maxWidth = 0;
    uint32_t m_maxHeight = 0;
    bool m_videoDecoderModuleLoaded = false;
    bool m_initialized = false;
    std::string m_lastError;
};

} // namespace xcloud
