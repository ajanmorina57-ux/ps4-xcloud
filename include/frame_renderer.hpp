#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace xcloud {

bool renderRgbaFrame(
    const uint8_t* source,
    uint32_t sourceWidth,
    uint32_t sourceHeight,
    size_t sourceStride,
    uint8_t* destination,
    uint32_t destinationWidth,
    uint32_t destinationHeight,
    size_t destinationStride,
    std::string& error);

} // namespace xcloud
