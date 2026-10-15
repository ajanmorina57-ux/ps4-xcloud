#include "video_decoder.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace xcloud {

PS4AvcDecoder::PS4AvcDecoder() {}

PS4AvcDecoder::~PS4AvcDecoder() {
    shutdown();
}

bool PS4AvcDecoder::init(uint32_t width, uint32_t height) {
#if defined(__ORBIS__) || defined(__PS4__)
    (void)width;
    (void)height;
    fprintf(stderr, "[Decoder] PS4 AVC decoder API is not implemented yet.\n");
    return false;
#else
    (void)width;
    (void)height;
    m_initialized = true;
    return true;
#endif
}

bool PS4AvcDecoder::decodeFrame(const uint8_t* h264Data, size_t size, FrameInfo& outFrame) {
    if (!m_initialized) return false;
    // Decode Access Unit (AU) implementation will be wired to RTP depacketizer
    return true;
}

void PS4AvcDecoder::shutdown() {
    m_decoderHandle = nullptr;
    m_decoderMemory = nullptr;
    m_memorySize = 0;
    m_initialized = false;
}

} // namespace xcloud
