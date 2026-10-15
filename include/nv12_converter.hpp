#pragma once

#include "video_decoder.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace xcloud {

bool convertNv12ToRgba(
    const FrameInfo& frame,
    std::vector<uint8_t>& rgba,
    std::string& error);

} // namespace xcloud
