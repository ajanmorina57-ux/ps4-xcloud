#include "pad_input.hpp"
#include <cstring>

#if defined(__ORBIS__) || defined(__PS4__)
#include <orbis/Pad.h>
#include <orbis/UserService.h>
#endif

namespace xcloud {

PadManager::PadManager() {}

PadManager::~PadManager() {
    shutdown();
}

bool PadManager::init() {
#if defined(__ORBIS__) || defined(__PS4__)
    sceUserServiceInitialize(nullptr);
    sceUserServiceGetInitialUser(&m_userId);

    scePadInit();
    m_padHandle = scePadOpen(m_userId, ORBIS_PAD_PORT_TYPE_STANDARD, 0, nullptr);
    return m_padHandle >= 0;
#else
    return true; // Mock for host testing
#endif
}

bool PadManager::poll(XboxGamepadReport& outReport) {
    outReport = {};
    outReport.reportType = 0x01;

#if defined(__ORBIS__) || defined(__PS4__)
    if (m_padHandle < 0) return false;

    OrbisPadData data{};
    int ret = scePadReadState(m_padHandle, &data);
    if (ret != 0 || !data.connected) {
        return false;
    }

    // Map DualShock 4 buttons to Xbox schema:
    // DS4 Cross (0x4000) -> Xbox A
    if (data.buttons & ORBIS_PAD_BUTTON_CROSS) outReport.buttons |= (1 << 0);
    // DS4 Circle (0x2000) -> Xbox B
    if (data.buttons & ORBIS_PAD_BUTTON_CIRCLE) outReport.buttons |= (1 << 1);
    // DS4 Square (0x8000) -> Xbox X
    if (data.buttons & ORBIS_PAD_BUTTON_SQUARE) outReport.buttons |= (1 << 2);
    // DS4 Triangle (0x1000) -> Xbox Y
    if (data.buttons & ORBIS_PAD_BUTTON_TRIANGLE) outReport.buttons |= (1 << 3);

    // Bumpers
    if (data.buttons & ORBIS_PAD_BUTTON_L1) outReport.buttons |= (1 << 4);
    if (data.buttons & ORBIS_PAD_BUTTON_R1) outReport.buttons |= (1 << 5);

    // Menu / View
    if (data.buttons & ORBIS_PAD_BUTTON_OPTIONS) outReport.buttons |= (1 << 6); // Start / Menu
    if (data.buttons & ORBIS_PAD_BUTTON_TOUCH_PAD) outReport.buttons |= (1 << 7); // Back / View

    // Stick clicks
    if (data.buttons & ORBIS_PAD_BUTTON_L3) outReport.buttons |= (1 << 8);
    if (data.buttons & ORBIS_PAD_BUTTON_R3) outReport.buttons |= (1 << 9);

    // D-Pad
    if (data.buttons & ORBIS_PAD_BUTTON_UP) outReport.buttons |= (1 << 10);
    if (data.buttons & ORBIS_PAD_BUTTON_DOWN) outReport.buttons |= (1 << 11);
    if (data.buttons & ORBIS_PAD_BUTTON_LEFT) outReport.buttons |= (1 << 12);
    if (data.buttons & ORBIS_PAD_BUTTON_RIGHT) outReport.buttons |= (1 << 13);

    // Triggers (Analog 0..255)
    outReport.leftTrigger = data.analogButtons.l2;
    outReport.rightTrigger = data.analogButtons.r2;

    // Analog Sticks (PS4: 0..255 -> Xbox: -32768..32767)
    outReport.thumbLX = (int16_t)(((int)data.leftStick.x - 128) * 256);
    outReport.thumbLY = (int16_t)(((int)(255 - data.leftStick.y) - 128) * 256);
    outReport.thumbRX = (int16_t)(((int)data.rightStick.x - 128) * 256);
    outReport.thumbRY = (int16_t)(((int)(255 - data.rightStick.y) - 128) * 256);

    return true;
#else
    return true;
#endif
}

void PadManager::shutdown() {
#if defined(__ORBIS__) || defined(__PS4__)
    if (m_padHandle >= 0) {
        scePadClose(m_padHandle);
        m_padHandle = -1;
    }
#endif
}

} // namespace xcloud
