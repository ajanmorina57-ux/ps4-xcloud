#include "frame_renderer.hpp"

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

namespace {

void testAspectFitAndLetterboxing() {
    const std::vector<uint8_t> source{
        255, 0, 0, 255,
        0, 255, 0, 255};
    std::vector<uint8_t> destination(2 * 2 * 4, 9);
    std::string error;
    assert(xcloud::renderRgbaFrame(
        source.data(), 2, 1, 8,
        destination.data(), 2, 2, 8,
        error));
    assert(error.empty());
    assert((destination == std::vector<uint8_t>{
        255, 0, 0, 255, 0, 255, 0, 255,
        0, 0, 0, 255, 0, 0, 0, 255}));
}

void testUpscalingAndPaddedRows() {
    const std::vector<uint8_t> source{
        10, 20, 30, 255, 99, 99, 99, 99,
        40, 50, 60, 255, 88, 88, 88, 88};
    std::vector<uint8_t> destination(2 * 4 * 4, 0);
    std::string error;
    assert(xcloud::renderRgbaFrame(
        source.data(), 1, 2, 8,
        destination.data(), 2, 4, 8,
        error));
    assert((destination == std::vector<uint8_t>{
        10, 20, 30, 255, 10, 20, 30, 255,
        10, 20, 30, 255, 10, 20, 30, 255,
        40, 50, 60, 255, 40, 50, 60, 255,
        40, 50, 60, 255, 40, 50, 60, 255}));
}

void testRejectsInvalidInput() {
    const uint8_t pixel[] = {1, 2, 3, 4};
    uint8_t output[4]{};
    std::string error;
    assert(!xcloud::renderRgbaFrame(
        nullptr, 1, 1, 4, output, 1, 1, 4, error));
    assert(!error.empty());
    assert(!xcloud::renderRgbaFrame(
        pixel, 1, 1, 3, output, 1, 1, 4, error));
    assert(!error.empty());
}

} // namespace

int main() {
    testAspectFitAndLetterboxing();
    testUpscalingAndPaddedRows();
    testRejectsInvalidInput();
}
