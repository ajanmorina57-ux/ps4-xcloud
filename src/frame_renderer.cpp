#include "frame_renderer.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace xcloud {
namespace {

constexpr uint32_t MaxFrameDimension = 3840;

} // namespace

bool renderRgbaFrame(
    const uint8_t* source,
    uint32_t sourceWidth,
    uint32_t sourceHeight,
    size_t sourceStride,
    uint8_t* destination,
    uint32_t destinationWidth,
    uint32_t destinationHeight,
    size_t destinationStride,
    std::string& error) {
    error.clear();
    if (source == nullptr || destination == nullptr) {
        error = "RGBA frame source or destination is missing.";
        return false;
    }
    if (sourceWidth == 0 || sourceHeight == 0 ||
        destinationWidth == 0 || destinationHeight == 0 ||
        sourceWidth > MaxFrameDimension || sourceHeight > MaxFrameDimension ||
        destinationWidth > MaxFrameDimension || destinationHeight > MaxFrameDimension) {
        error = "RGBA frame dimensions are invalid or exceed the supported limit.";
        return false;
    }
    if (sourceStride < static_cast<size_t>(sourceWidth) * 4 ||
        destinationStride < static_cast<size_t>(destinationWidth) * 4 ||
        sourceStride > std::numeric_limits<size_t>::max() / sourceHeight ||
        destinationStride > std::numeric_limits<size_t>::max() / destinationHeight) {
        error = "RGBA frame row stride is invalid or unaddressable.";
        return false;
    }

    uint32_t renderedWidth = destinationWidth;
    uint32_t renderedHeight = destinationHeight;
    if (static_cast<uint64_t>(sourceWidth) * destinationHeight >
        static_cast<uint64_t>(sourceHeight) * destinationWidth) {
        renderedHeight = static_cast<uint32_t>(
            static_cast<uint64_t>(sourceHeight) * destinationWidth / sourceWidth);
    } else {
        renderedWidth = static_cast<uint32_t>(
            static_cast<uint64_t>(sourceWidth) * destinationHeight / sourceHeight);
    }
    renderedWidth = std::max(renderedWidth, 1u);
    renderedHeight = std::max(renderedHeight, 1u);

    const uint32_t left = (destinationWidth - renderedWidth) / 2;
    const uint32_t top = (destinationHeight - renderedHeight) / 2;
    for (uint32_t y = 0; y < destinationHeight; ++y) {
        uint8_t* destinationRow = destination + static_cast<size_t>(y) * destinationStride;
        for (uint32_t x = 0; x < destinationWidth; ++x) {
            uint8_t* pixel = destinationRow + static_cast<size_t>(x) * 4;
            pixel[0] = 0;
            pixel[1] = 0;
            pixel[2] = 0;
            pixel[3] = 255;
        }
    }

    for (uint32_t y = 0; y < renderedHeight; ++y) {
        const uint32_t sourceY = static_cast<uint32_t>(
            static_cast<uint64_t>(y) * sourceHeight / renderedHeight);
        const uint8_t* sourceRow = source + static_cast<size_t>(sourceY) * sourceStride;
        uint8_t* destinationRow =
            destination + static_cast<size_t>(top + y) * destinationStride +
            static_cast<size_t>(left) * 4;
        for (uint32_t x = 0; x < renderedWidth; ++x) {
            const uint32_t sourceX = static_cast<uint32_t>(
                static_cast<uint64_t>(x) * sourceWidth / renderedWidth);
            std::memcpy(
                destinationRow + static_cast<size_t>(x) * 4,
                sourceRow + static_cast<size_t>(sourceX) * 4,
                4);
        }
    }
    return true;
}

} // namespace xcloud
