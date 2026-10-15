#pragma once
#include <cstdint>

namespace xcloud {

// Standard Xbox Controller report format for xCloud WebRTC data channel
#pragma pack(push, 1)
struct XboxGamepadReport {
    uint8_t reportType = 0x01; // Gamepad data
    uint16_t buttons;          // A, B, X, Y, Dpad, LB, RB, Start, Back, Guide
    uint8_t leftTrigger;       // 0 - 255
    uint8_t rightTrigger;      // 0 - 255
    int16_t thumbLX;           // -32768 to 32767
    int16_t thumbLY;           // -32768 to 32767
    int16_t thumbRX;           // -32768 to 32767
    int16_t thumbRY;           // -32768 to 32767
};
#pragma pack(pop)

class PadManager {
public:
    PadManager();
    ~PadManager();

    bool init();
    bool poll(XboxGamepadReport& outReport);
    void shutdown();

private:
    int32_t m_padHandle = -1;
    int32_t m_userId = 0;
};

} // namespace xcloud
