#include <cstdio>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include "auth.hpp"
#include "pad_input.hpp"
#include "video_decoder.hpp"
#include "http_client.hpp"
#include "display.hpp"
#include "xcloud_session.hpp"

#ifndef XCLOUD_CLIENT_ID
#define XCLOUD_CLIENT_ID ""
#endif

namespace {

void drawTextLimited(
    xcloud::DisplayEngine& display,
    const std::string& text,
    int x,
    int y,
    int scale,
    int maxWidth,
    xcloud::PixelColor color) {
    const size_t maxChars = static_cast<size_t>(maxWidth / (9 * scale));
    if (text.size() <= maxChars) {
        display.drawText(text, x, y, scale, color);
        return;
    }
    if (maxChars <= 3) return;
    display.drawText(text.substr(0, maxChars - 3) + "...", x, y, scale, color);
}

void drawTextWrapped(
    xcloud::DisplayEngine& display,
    const std::string& text,
    int x,
    int y,
    int scale,
    int maxWidth,
    int maxLines,
    xcloud::PixelColor color) {
    const size_t maxChars = static_cast<size_t>(maxWidth / (9 * scale));
    if (maxChars == 0 || maxLines <= 0) return;

    size_t offset = 0;
    for (int line = 0; line < maxLines && offset < text.size(); ++line) {
        size_t end = std::min(offset + maxChars, text.size());
        if (end < text.size()) {
            const size_t space = text.rfind(' ', end);
            if (space != std::string::npos && space >= offset) end = space;
        }
        if (end == offset) end = std::min(offset + maxChars, text.size());
        display.drawText(text.substr(offset, end - offset), x, y + line * 18, scale, color);
        offset = end;
        while (offset < text.size() && text[offset] == ' ') ++offset;
    }
    if (offset < text.size() && maxLines > 0) {
        const size_t remaining = std::min(maxChars, text.size() - offset);
        if (remaining > 3) {
            display.drawText(text.substr(offset, remaining - 3) + "...",
                             x,
                             y + (maxLines - 1) * 18,
                             scale,
                             color);
        }
    }
}

void drawStep(
    xcloud::DisplayEngine& display,
    const char* number,
    const char* title,
    const char* description,
    int y,
    xcloud::PixelColor green,
    xcloud::PixelColor white,
    xcloud::PixelColor muted) {
    display.fillRect(160, y, 48, 48, xcloud::PixelColor::FromRGB(28, 42, 36));
    display.drawBorder(160, y, 48, 48, 2, green);
    display.drawText(number, 175, y + 12, 2, green);
    display.drawText(title, 232, y + 2, 2, white);
    display.drawText(description, 232, y + 30, 1, muted);
}

void drawBadge(
    xcloud::DisplayEngine& display,
    int x,
    int y,
    int width,
    const std::string& label,
    xcloud::PixelColor color,
    xcloud::PixelColor surface) {
    display.fillRect(x, y, width, 34, surface);
    display.drawBorder(x, y, width, 34, 1, color);
    display.fillRect(x + 12, y + 13, 8, 8, color);
    drawTextLimited(display, label, x + 30, y + 11, 1, width - 42, color);
}

} // namespace

int main(int argc, char* argv[]) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("[xCloud-PS4] Starting...\n");

#if defined(XCLOUD_BOOT_PROBE)
    printf("[xCloud-PS4] Boot probe active; PS4 service initialization is disabled.\n");
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
#elif defined(XCLOUD_DISPLAY_PROBE)
    printf("[xCloud-PS4] Display probe active; controller, decoder, and network are disabled.\n");
    xcloud::DisplayEngine display(1920, 1080);
    bool displayOk = display.init();
    printf("[xCloud-PS4] Display initialization %s.\n", displayOk ? "complete" : "failed");
#if defined(XCLOUD_PRESENT_PROBE)
    if (displayOk) {
        printf("[xCloud-PS4] Drawing and presenting a test screen.\n");
        display.clear(xcloud::PixelColor::XboxDark());
        display.fillRect(0, 0, 1920, 100, xcloud::PixelColor::XboxGreen());
        display.drawText("DRAW TEST", 60, 32, 4, xcloud::PixelColor::White());
    }
#endif
    while (true) {
#if defined(XCLOUD_PRESENT_PROBE)
        if (displayOk) display.present();
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
#else
    // 1. Initialize Display Engine
    xcloud::DisplayEngine display(1920, 1080);
    if (!display.init()) {
        printf("[xCloud-PS4] Display initialization warning.\n");
    } else {
        printf("[xCloud-PS4] Display Engine (1080p 60Hz) ready.\n");
    }

    xcloud::PadManager pad;
    xcloud::PS4AvcDecoder decoder;
    printf("[xCloud-PS4] Video decoder initialization deferred until a stream starts.\n");

    xcloud::AuthManager auth(XCLOUD_CLIENT_ID);
    xcloud::DeviceCodeResponse devCode;
    struct AuthUiState {
        bool codeRequested = false;
        bool loggedIn = false;
        bool cloudCatalogReady = false;
        bool fortniteAvailable = false;
        bool sessionStarted = false;
        bool transferCodeRequested = false;
        std::string userCode;
        std::string transferCode;
        std::string gamertag;
        std::string cloudStatus;
        std::string fortniteTitleId;
        std::string sessionStatus;
        size_t cloudTitleCount = 0;
        std::string status = XCLOUD_CLIENT_ID[0] == '\0'
            ? "Set XCLOUD_CLIENT_ID in CMake to enable sign-in."
            : "Connecting to Microsoft...";
    };
    AuthUiState authUi;
    std::mutex authUiMutex;
    std::atomic<bool> stopAuth{false};
    std::thread authThread;
    std::thread sessionThread;
    xcloud::XcloudSession cloudSession;
    bool padOk = false;
    std::atomic<bool> netOk{false};
    bool authThreadStarted = false;
    bool crossWasDown = false;

    bool running = true;

    while (running) {
        // Poll Controller
        xcloud::XboxGamepadReport report;
        if (pad.poll(report)) {
            // Exit combo: OPTIONS + TOUCHPAD
            if ((report.buttons & (1 << 6)) && (report.buttons & (1 << 7))) {
                printf("[xCloud-PS4] Exit combo detected. Quitting...\n");
                running = false;
            }
            const bool crossDown = (report.buttons & (1 << 0)) != 0;
            if (crossDown && !crossWasDown) {
                std::string titleId;
                {
                    std::lock_guard<std::mutex> lock(authUiMutex);
                    if (authUi.cloudCatalogReady && authUi.fortniteAvailable && !authUi.sessionStarted) {
                        authUi.sessionStarted = true;
                        authUi.sessionStatus = "Starting Fortnite session...";
                        titleId = authUi.fortniteTitleId;
                    }
                }
                if (!titleId.empty()) {
                    sessionThread = std::thread([&, titleId]() {
                        auto updateStatus = [&](const std::string& status) {
                            std::lock_guard<std::mutex> lock(authUiMutex);
                            authUi.sessionStatus = status;
                        };

                        std::string error;
                        if (!cloudSession.startSession(titleId, error)) {
                            updateStatus(error);
                            return;
                        }

                        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(3);
                        while (!stopAuth.load() && std::chrono::steady_clock::now() < deadline) {
                            std::string state;
                            if (!cloudSession.getSessionState(state, error)) {
                                updateStatus(error);
                                cloudSession.stopSession();
                                return;
                            }
                            if (state == "WaitingForResources") {
                                updateStatus("Fortnite is waiting in the cloud queue.");
                            } else if (state == "Provisioning") {
                                updateStatus("Fortnite session is provisioning...");
                            } else if (state == "ReadyToConnect") {
                                updateStatus("Requesting cloud connection sign-in...");
                                xcloud::DeviceCodeResponse transferCode;
                                if (!auth.requestConsoleTransferCode(transferCode)) {
                                    updateStatus("Transfer code request failed: " + auth.getLastError());
                                    cloudSession.stopSession();
                                    return;
                                }
                                {
                                    std::lock_guard<std::mutex> lock(authUiMutex);
                                    authUi.transferCodeRequested = true;
                                    authUi.transferCode = transferCode.user_code;
                                    authUi.sessionStatus = "Approve the connection on another device.";
                                }

                                std::string transferToken;
                                const int transferTimeout = std::max(
                                    1,
                                    static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                                        deadline - std::chrono::steady_clock::now()).count()));
                                if (!auth.pollForConsoleTransferToken(
                                        transferCode.device_code,
                                        transferCode.interval,
                                        std::min(transferCode.expires_in, transferTimeout),
                                        transferToken,
                                        &stopAuth)) {
                                    {
                                        std::lock_guard<std::mutex> lock(authUiMutex);
                                        authUi.transferCodeRequested = false;
                                        authUi.transferCode.clear();
                                    }
                                    if (!stopAuth.load()) {
                                        updateStatus("Transfer sign-in failed: " + auth.getLastError());
                                    }
                                    cloudSession.stopSession();
                                    return;
                                }
                                {
                                    std::lock_guard<std::mutex> lock(authUiMutex);
                                    authUi.transferCodeRequested = false;
                                    authUi.transferCode.clear();
                                }
                                if (!cloudSession.connectSession(transferToken, error)) {
                                    updateStatus(error);
                                    cloudSession.stopSession();
                                    return;
                                }
                                updateStatus("Session handoff accepted; WebRTC is next.");
                                cloudSession.stopSession();
                                return;
                            } else if (state == "Provisioned") {
                                updateStatus("Session provisioned; WebRTC is not connected.");
                                cloudSession.stopSession();
                                return;
                            } else {
                                updateStatus("Cloud session state: " + state);
                            }
                            std::this_thread::sleep_for(std::chrono::seconds(3));
                        }

                        if (!stopAuth.load()) updateStatus("Cloud session timed out; try again later.");
                        cloudSession.stopSession();
                    });
                }
            }
            crossWasDown = crossDown;
        }

        AuthUiState authSnapshot;
        {
            std::lock_guard<std::mutex> lock(authUiMutex);
            authSnapshot = authUi;
        }
        const xcloud::PixelColor background = xcloud::PixelColor::FromRGB(10, 14, 15);
        const xcloud::PixelColor panel = xcloud::PixelColor::FromRGB(20, 27, 27);
        const xcloud::PixelColor raised = xcloud::PixelColor::FromRGB(13, 19, 19);
        const xcloud::PixelColor green = xcloud::PixelColor::FromRGB(72, 206, 123);
        const xcloud::PixelColor greenDim = xcloud::PixelColor::FromRGB(27, 59, 43);
        const xcloud::PixelColor white = xcloud::PixelColor::FromRGB(239, 245, 241);
        const xcloud::PixelColor muted = xcloud::PixelColor::FromRGB(153, 168, 161);
        const xcloud::PixelColor gold = xcloud::PixelColor::FromRGB(246, 199, 91);
        const xcloud::PixelColor line = xcloud::PixelColor::FromRGB(49, 61, 57);

        display.clear(background);

        display.fillRect(0, 0, 1920, 5, green);
        display.fillRect(0, 5, 1920, 3, greenDim);
        display.fillRect(112, 35, 54, 54, green);
        display.drawText("X", 129, 47, 4, background);
        display.drawText("XBOX CLOUD", 188, 38, 4, white);
        display.drawText("GAME STREAMING CLIENT", 190, 78, 1, muted);
        drawBadge(
            display,
            1530,
            43,
            280,
            "PS4 HOMEBREW",
            green,
            xcloud::PixelColor::FromRGB(17, 26, 22));
        display.fillRect(110, 120, 1700, 1, line);

        display.fillRect(110, 157, 1700, 746, panel);
        display.fillRect(110, 157, 6, 746, green);
        display.fillRect(1125, 195, 1, 670, line);

        drawBadge(display, 160, 196, 206, "CLOUD GAMING", green, greenDim);
        if (!authSnapshot.loggedIn) {
            display.drawText("YOUR GAMES.", 156, 265, 5, white);
            display.drawText("YOUR SCREEN.", 156, 323, 5, green);
            display.drawText("A familiar Xbox experience, ready for your living room.", 160, 405, 2, muted);
            display.fillRect(160, 444, 880, 1, line);

            display.fillRect(183, 538, 2, 80, line);
            display.fillRect(183, 618, 2, 80, line);

            drawStep(
                display,
                "01",
                "OPEN THE SIGN-IN PAGE",
                "Use a phone or computer to visit microsoft.com/link.",
                490,
                green,
                white,
                muted);
            drawStep(
                display,
                "02",
                "ENTER YOUR DEVICE CODE",
                "Approve the sign-in with your Microsoft account.",
                570,
                green,
                white,
                muted);
            drawStep(
                display,
                "03",
                "PICK UP WHERE YOU LEFT OFF",
                "Cloud saves stay with your Xbox profile.",
                650,
                green,
                white,
                muted);

            display.fillRect(1170, 195, 590, 670, raised);
            display.drawBorder(1170, 195, 590, 670, 1, line);
            display.drawText("ACCOUNT", 1210, 225, 1, muted);
            display.drawText("LINK YOUR ACCOUNT", 1210, 250, 2, white);
            display.fillRect(1210, 290, 510, 1, line);

            if (authSnapshot.codeRequested) {
                drawBadge(display, 1210, 316, 178, "WAITING", green, greenDim);
                display.drawText("OPEN THIS ADDRESS", 1210, 375, 1, muted);
                display.drawText("microsoft.com/link", 1210, 398, 2, white);
                display.drawText("THEN ENTER YOUR CODE", 1210, 466, 1, muted);
                display.fillRect(1210, 492, 510, 105, background);
                display.drawBorder(1210, 492, 510, 105, 1, green);
                drawTextLimited(display, authSnapshot.userCode, 1240, 518, 5, 450, white);
                drawTextLimited(display, authSnapshot.status, 1210, 620, 1, 510, muted);
            } else if (XCLOUD_CLIENT_ID[0] == '\0') {
                drawBadge(display, 1210, 316, 230, "ACTION NEEDED", gold, xcloud::PixelColor::FromRGB(46, 37, 22));
                display.drawText("SIGN-IN IS NOT SET UP", 1210, 382, 2, white);
                display.drawText("This build needs an app ID", 1210, 430, 1, muted);
                display.drawText("before Microsoft sign-in can", 1210, 452, 1, muted);
                display.drawText("be enabled.", 1210, 474, 1, muted);
                display.fillRect(1210, 528, 510, 85, background);
                display.drawBorder(1210, 528, 510, 85, 1, line);
                display.drawText("NEXT STEP", 1230, 545, 1, green);
                display.drawText("Create an app registration", 1230, 570, 1, white);
                display.drawText("and rebuild with its client ID.", 1230, 592, 1, muted);
            } else {
                const bool signInFailed =
                    authSnapshot.status.find("failed") != std::string::npos ||
                    authSnapshot.status.find("expired") != std::string::npos;
                if (signInFailed) {
                    drawBadge(display, 1210, 316, 214, "ATTENTION", gold, xcloud::PixelColor::FromRGB(46, 37, 22));
                    display.drawText("SIGN-IN FAILED", 1210, 382, 2, white);
                    display.fillRect(1210, 425, 510, 150, background);
                    display.drawBorder(1210, 425, 510, 150, 1, line);
                    drawTextLimited(display, authSnapshot.status, 1230, 450, 1, 470, muted);
                } else {
                    drawBadge(display, 1210, 316, 206, "CONNECTING", gold, xcloud::PixelColor::FromRGB(46, 37, 22));
                    display.drawText("PREPARING SIGN-IN", 1210, 382, 2, white);
                    display.fillRect(1210, 425, 510, 100, background);
                    display.drawBorder(1210, 425, 510, 100, 1, line);
                    display.drawText("....", 1435, 449, 4, green);
                    drawTextLimited(display, authSnapshot.status, 1210, 550, 1, 510, muted);
                }
            }
            display.fillRect(1210, 720, 510, 1, line);
            display.drawText("GAME PASS MAY BE REQUIRED", 1210, 748, 1, muted);
        } else {
            drawBadge(display, 160, 196, 230, "ACCOUNT CONNECTED", green, greenDim);
            display.drawText("WELCOME BACK.", 156, 280, 5, white);
            const std::string accountLabel = authSnapshot.gamertag.empty()
                ? "XBOX ACCOUNT CONNECTED"
                : "Gamertag: " + authSnapshot.gamertag;
            drawTextLimited(display, accountLabel, 160, 370, 3, 890, green);
            display.fillRect(160, 425, 880, 1, line);
            drawTextLimited(
                display,
                authSnapshot.cloudStatus.empty()
                    ? "YOUR XBOX PROFILE IS READY"
                    : authSnapshot.cloudStatus,
                160,
                470,
                1,
                890,
                muted);

            display.fillRect(1170, 195, 590, 670, raised);
            display.drawBorder(1170, 195, 590, 670, 1, line);
            display.drawText("NEXT UP", 1210, 225, 1, muted);
            if (authSnapshot.cloudCatalogReady) {
                if (authSnapshot.fortniteAvailable) {
                    drawBadge(display, 1210, 280, 244, "FORTNITE READY", green, greenDim);
                    display.drawText("CROSS: START SESSION", 1210, 350, 2, white);
                    const std::string titleCount = std::to_string(authSnapshot.cloudTitleCount) + " TITLES FOUND";
                    display.drawText(titleCount.c_str(), 1210, 390, 1, muted);
                    if (!authSnapshot.sessionStatus.empty()) {
                        display.fillRect(1210, 440, 510, 90, background);
                        display.drawBorder(1210, 440, 510, 90, 1, line);
                        drawTextWrapped(display, authSnapshot.sessionStatus, 1230, 453, 1, 470, 4, muted);
                    }
                    if (authSnapshot.transferCodeRequested) {
                        display.drawText("APPROVE AT MICROSOFT.COM/LINK", 1210, 555, 1, muted);
                        display.fillRect(1210, 580, 510, 90, background);
                        display.drawBorder(1210, 580, 510, 90, 1, green);
                        drawTextLimited(display, authSnapshot.transferCode, 1240, 595, 5, 450, white);
                    }
                } else {
                    drawBadge(display, 1210, 280, 244, "CLOUD CATALOG READY", green, greenDim);
                    display.drawText("FORTNITE ID NOT MATCHED", 1210, 350, 2, white);
                    display.drawText("Session catalog mapping incomplete.", 1210, 390, 1, muted);
                }
            } else if (!authSnapshot.cloudStatus.empty()) {
                drawBadge(display, 1210, 280, 244, "CLOUD LOGIN ISSUE", gold, xcloud::PixelColor::FromRGB(46, 37, 22));
                drawTextLimited(display, authSnapshot.cloudStatus, 1210, 350, 1, 510, muted);
            } else {
                display.drawText("STREAMING IS", 1210, 300, 3, white);
                display.drawText("NOT AVAILABLE YET", 1210, 345, 3, gold);
            }
            display.fillRect(1210, 440, 510, 1, line);
            if (!authSnapshot.cloudCatalogReady && authSnapshot.cloudStatus.empty()) {
                display.drawText("Game-session setup and", 1210, 480, 1, muted);
                display.drawText("video decoding are still", 1210, 502, 1, muted);
                display.drawText("being developed.", 1210, 524, 1, muted);
            }
        }

        display.fillRect(110, 935, 1700, 1, line);
        drawBadge(
            display,
            110,
            955,
            270,
            padOk ? "CONTROLLER READY" : "CONTROLLER OFFLINE",
            padOk ? green : gold,
            padOk ? greenDim : xcloud::PixelColor::FromRGB(46, 37, 22));
        drawBadge(
            display,
            395,
            955,
            220,
            netOk.load() ? "NETWORK READY" : "NETWORK IDLE",
            netOk.load() ? green : muted,
            netOk.load() ? greenDim : raised);
        drawTextLimited(display, "OPTIONS + TOUCHPAD  EXIT", 1500, 966, 1, 300, muted);

        // Present Frame to TV (VSync)
        display.present();

        if (!authThreadStarted) {
            authThreadStarted = true;
            printf("[xCloud-PS4] Initializing controller...\n");
            padOk = pad.init();
            printf("[xCloud-PS4] Controller initialization %s.\n", padOk ? "complete" : "failed");

            if (XCLOUD_CLIENT_ID[0] != '\0') {
                authThread = std::thread([&]() {
                printf("[xCloud-PS4] Initializing network and HTTP...\n");
                netOk = xcloud::HttpClient::init();
                printf("[xCloud-PS4] Network and HTTP initialization %s.\n", netOk ? "complete" : "failed");
                if (!netOk) {
                    std::lock_guard<std::mutex> lock(authUiMutex);
                    authUi.status = "Network initialization failed.";
                    return;
                }

                {
                    std::lock_guard<std::mutex> lock(authUiMutex);
                    authUi.status = "Requesting sign-in code...";
                }
                if (!auth.requestDeviceCode(devCode)) {
                    std::lock_guard<std::mutex> lock(authUiMutex);
                    authUi.status = "Sign-in failed: " + auth.getLastError();
                    if (authUi.status.size() > 68) authUi.status.resize(68);
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(authUiMutex);
                    authUi.codeRequested = true;
                    authUi.userCode = devCode.user_code;
                    authUi.status = "Enter the code at microsoft.com/link";
                }

                if (!auth.pollForTokens(
                        devCode.device_code,
                        devCode.interval,
                        devCode.expires_in,
                        &stopAuth)) {
                    if (!stopAuth.load()) {
                        std::lock_guard<std::mutex> lock(authUiMutex);
                        authUi.status = "Sign-in expired or failed. Restart to try again.";
                    }
                    return;
                }

                {
                    std::lock_guard<std::mutex> lock(authUiMutex);
                    authUi.status = "Signed in. Checking Xbox account...";
                }
                if (!auth.authenticateXboxLive()) {
                    std::lock_guard<std::mutex> lock(authUiMutex);
                    authUi.status = "Xbox account authentication failed.";
                    return;
                }

                {
                    std::lock_guard<std::mutex> lock(authUiMutex);
                    authUi.gamertag = auth.getTokens().gamertag;
                    authUi.loggedIn = true;
                    authUi.status = "Xbox account authenticated.";
                    authUi.cloudStatus = "Connecting to cloud gaming...";
                }

                std::string cloudError;
                if (!cloudSession.login(auth.getTokens().xsts_token, cloudError)) {
                    std::lock_guard<std::mutex> lock(authUiMutex);
                    authUi.cloudStatus = cloudError;
                    if (authUi.cloudStatus.size() > 68) authUi.cloudStatus.resize(68);
                    return;
                }

                std::vector<xcloud::CloudTitle> cloudTitles;
                if (!cloudSession.listTitles(cloudTitles, cloudError)) {
                    std::lock_guard<std::mutex> lock(authUiMutex);
                    authUi.cloudStatus = cloudError;
                    if (authUi.cloudStatus.size() > 68) authUi.cloudStatus.resize(68);
                    return;
                }

                const auto fortnite = std::find_if(
                    cloudTitles.begin(),
                    cloudTitles.end(),
                    [](const xcloud::CloudTitle& title) {
                        return title.productId == "BT5P2X999VH2";
                    });
                std::lock_guard<std::mutex> lock(authUiMutex);
                authUi.cloudTitleCount = cloudTitles.size();
                authUi.cloudCatalogReady = true;
                if (fortnite != cloudTitles.end()) {
                    authUi.fortniteAvailable = true;
                    authUi.fortniteTitleId = fortnite->titleId;
                }
                authUi.cloudStatus = "Cloud catalog loaded.";
                });
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 FPS
    }

    // Cleanup
    stopAuth = true;
    if (sessionThread.joinable()) sessionThread.join();
    if (authThread.joinable()) authThread.join();
    display.shutdown();
    pad.shutdown();
    decoder.shutdown();
    if (netOk.load()) xcloud::HttpClient::shutdown();

    printf("[xCloud-PS4] Exited cleanly.\n");
    return 0;
#endif
}
