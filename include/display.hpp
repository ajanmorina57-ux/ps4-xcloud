#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

#if defined(__ORBIS__) || defined(__PS4__)
#include <orbis/_types/kernel.h>
#endif

namespace xcloud {

struct PixelColor {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 255;

    static PixelColor FromRGB(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
        return {r, g, b, a};
    }
    static PixelColor XboxGreen() { return {16, 124, 16, 255}; }
    static PixelColor XboxDark()  { return {14, 17, 17, 255}; }
    static PixelColor CardDark()  { return {26, 32, 32, 255}; }
    static PixelColor White()     { return {255, 255, 255, 255}; }
    static PixelColor Gray()      { return {150, 150, 150, 255}; }
    static PixelColor Yellow()    { return {255, 215, 0, 255}; }
};

class DisplayEngine {
public:
    DisplayEngine(int width = 1920, int height = 1080);
    ~DisplayEngine();

    bool init();
    void shutdown();

    void clear(PixelColor color = PixelColor::XboxDark());
    void fillRect(int x, int y, int w, int h, PixelColor color);
    void drawBorder(int x, int y, int w, int h, int thickness, PixelColor color);

    void drawChar(char c, int x, int y, int scale, PixelColor color);
    void drawText(const std::string& text, int x, int y, int scale, PixelColor color);

    // Swap back and front buffers and present to TV (V-Sync)
    void present();

    int getWidth() const { return m_width; }
    int getHeight() const { return m_height; }

private:
    int m_width;
    int m_height;
    int m_depth = 4; // 32-bit ARGB
    int m_videoHandle = -1;
#if defined(__ORBIS__) || defined(__PS4__)
    OrbisKernelEqueue m_flipQueue = 0;
#else
    int m_flipQueue = -1;
#endif
    int m_currentBuffer = 0;
    int m_frameId = 0;

    off_t m_directMemOff = 0;
    size_t m_directMemSize = 0;
    void* m_videoMem = nullptr;
    uint8_t* m_buffers[2] = {nullptr, nullptr};
    bool m_flipQueueCreated = false;
    bool m_buffersRegistered = false;
    bool m_initialized = false;
};

} // namespace xcloud
