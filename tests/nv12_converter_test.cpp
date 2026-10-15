#include "nv12_converter.hpp"

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

namespace {

void testLimitedRangeBlackAndWhite() {
    const std::vector<uint8_t> y{16, 235};
    const std::vector<uint8_t> uv{128, 128};
    const xcloud::FrameInfo frame{2, 1, 2, 2, y.data(), uv.data()};
    std::vector<uint8_t> rgba;
    std::string error;
    assert(xcloud::convertNv12ToRgba(frame, rgba, error));
    assert(error.empty());
    assert((rgba == std::vector<uint8_t>{
        0, 0, 0, 255,
        255, 255, 255, 255}));
}

void testPaddedRowsAndSharedChroma() {
    const std::vector<uint8_t> y{
        81, 81, 0, 0,
        81, 81, 0, 0};
    const std::vector<uint8_t> uv{90, 240, 0, 0};
    const xcloud::FrameInfo frame{2, 2, 4, 4, y.data(), uv.data()};
    std::vector<uint8_t> rgba;
    std::string error;
    assert(xcloud::convertNv12ToRgba(frame, rgba, error));
    assert(rgba.size() == 16);
    for (size_t offset = 0; offset < rgba.size(); offset += 4) {
        assert(rgba[offset] > 240);
        assert(rgba[offset + 1] < 10);
        assert(rgba[offset + 2] < 10);
        assert(rgba[offset + 3] == 255);
    }
}

void testOddWidthUsesRoundedChromaRow() {
    const std::vector<uint8_t> y{16, 16, 16};
    const std::vector<uint8_t> uv{128, 128, 128, 128};
    const xcloud::FrameInfo frame{3, 1, 3, 4, y.data(), uv.data()};
    std::vector<uint8_t> rgba;
    std::string error;
    assert(xcloud::convertNv12ToRgba(frame, rgba, error));
    assert(rgba.size() == 12);
}

void testRejectsInvalidPlanesAndPitches() {
    std::vector<uint8_t> rgba;
    std::string error;
    const std::vector<uint8_t> plane(8, 128);

    const xcloud::FrameInfo missingPlane{2, 2, 2, 2, nullptr, plane.data()};
    assert(!xcloud::convertNv12ToRgba(missingPlane, rgba, error));
    assert(!error.empty());

    const xcloud::FrameInfo shortPitch{3, 1, 2, 2, plane.data(), plane.data()};
    assert(!xcloud::convertNv12ToRgba(shortPitch, rgba, error));
    assert(rgba.empty());
    assert(!error.empty());
}

} // namespace

int main() {
    testLimitedRangeBlackAndWhite();
    testPaddedRowsAndSharedChroma();
    testOddWidthUsesRoundedChromaRow();
    testRejectsInvalidPlanesAndPitches();
}
