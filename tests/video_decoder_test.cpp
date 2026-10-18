#include "video_decoder.hpp"

#include <cassert>
#include <cstdint>

int main() {
    xcloud::PS4AvcDecoder decoder;
    assert(!decoder.init(1280, 720));

    const uint8_t accessUnit[] = {0, 0, 0, 1, 0x65};
    xcloud::FrameInfo frame{1, 2, 3, 4, accessUnit, accessUnit};
    assert(!decoder.decodeFrame(accessUnit, sizeof(accessUnit), frame));
    assert(frame.width == 0);
    assert(frame.height == 0);
    assert(frame.pitchY == 0);
    assert(frame.pitchUV == 0);
    assert(frame.planeY == nullptr);
    assert(frame.planeUV == nullptr);
}
