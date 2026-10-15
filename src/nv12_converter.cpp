#include "nv12_converter.hpp"

#include <algorithm>
#include <limits>

namespace xcloud {
namespace {

constexpr uint32_t MaxFrameWidth = 3840;
constexpr uint32_t MaxFrameHeight = 2160;

uint8_t clampByte(int value) {
    return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

} // namespace

bool convertNv12ToRgba(
    const FrameInfo& frame,
    std::vector<uint8_t>& rgba,
    std::string& error) {
    rgba.clear();
    error.clear();
    if (frame.width == 0 || frame.height == 0 ||
        frame.width > MaxFrameWidth || frame.height > MaxFrameHeight) {
        error = "NV12 frame dimensions are invalid or exceed the supported limit.";
        return false;
    }
    if (frame.planeY == nullptr || frame.planeUV == nullptr) {
        error = "NV12 frame is missing a Y or UV plane.";
        return false;
    }

    const uint32_t chromaRowBytes = ((frame.width + 1) / 2) * 2;
    if (frame.pitchY < frame.width || frame.pitchUV < chromaRowBytes) {
        error = "NV12 plane pitch is smaller than the frame dimensions.";
        return false;
    }

    const size_t width = frame.width;
    const size_t height = frame.height;
    if (width > std::numeric_limits<size_t>::max() / height / 4) {
        error = "NV12 output size exceeds the addressable limit.";
        return false;
    }
    rgba.resize(width * height * 4);

    for (uint32_t y = 0; y < frame.height; ++y) {
        const uint8_t* yRow = frame.planeY + static_cast<size_t>(y) * frame.pitchY;
        const uint8_t* uvRow = frame.planeUV + static_cast<size_t>(y / 2) * frame.pitchUV;
        for (uint32_t x = 0; x < frame.width; ++x) {
            const int luminance = std::max(0, static_cast<int>(yRow[x]) - 16);
            const size_t chromaOffset = static_cast<size_t>(x / 2) * 2;
            const int u = static_cast<int>(uvRow[chromaOffset]) - 128;
            const int v = static_cast<int>(uvRow[chromaOffset + 1]) - 128;
            const size_t outputOffset = (static_cast<size_t>(y) * width + x) * 4;
            rgba[outputOffset] = clampByte((298 * luminance + 409 * v + 128) >> 8);
            rgba[outputOffset + 1] = clampByte((298 * luminance - 100 * u - 208 * v + 128) >> 8);
            rgba[outputOffset + 2] = clampByte((298 * luminance + 516 * u + 128) >> 8);
            rgba[outputOffset + 3] = 255;
        }
    }
    return true;
}

} // namespace xcloud
