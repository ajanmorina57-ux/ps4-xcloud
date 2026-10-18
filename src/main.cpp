#include <cstdio>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <mutex>
#include <thread>
#include <string>
#include <vector>
#include <sys/stat.h>

#include "auth.hpp"
#include "pad_input.hpp"
#include "video_decoder.hpp"
#include "http_client.hpp"
#include "display.hpp"
#include "xcloud_session.hpp"
#include "network_probe.hpp"
#include "webrtc_probe.hpp"
#include "pipeline.hpp"

#ifndef XCLOUD_CLIENT_ID
#define XCLOUD_CLIENT_ID ""
#endif

namespace {

constexpr const char* AppBuildVersion = "01.16";
constexpr const char* StreamStagePath = "/data/ps4-xcloud/last_stream_stage.txt";

void saveLastStreamStage(const std::string& stage) {
    if (stage.empty()) return;
    mkdir("/data/ps4-xcloud", 0777);
    FILE* file = std::fopen(StreamStagePath, "w");
    if (!file) return;
    std::fwrite(stage.data(), 1, stage.size(), file);
    std::fputc('\n', file);
    std::fclose(file);
}

std::string loadLastStreamStage() {
    FILE* file = std::fopen(StreamStagePath, "r");
    if (!file) return {};
    char stage[192]{};
    const size_t count = std::fread(stage, 1, sizeof(stage) - 1, file);
    std::fclose(file);
    while (count > 0 && (stage[count - 1] == '\n' || stage[count - 1] == '\r')) stage[count - 1] = '\0';
    return stage;
}

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

void drawCloudMark(
    xcloud::DisplayEngine& display,
    int x,
    int y,
    int unit,
    xcloud::PixelColor cloud,
    xcloud::PixelColor play) {
    constexpr int RowStarts[] = {3, 2, 1, 0, 0, 0, 1, 2};
    constexpr int RowEnds[] = {5, 6, 7, 8, 8, 8, 7, 6};
    constexpr int PlayStarts[] = {-1, -1, 4, 4, 4, 4, 4, -1};
    constexpr int PlayEnds[] = {-1, -1, 4, 5, 6, 5, 4, -1};
    for (int row = 0; row < 8; ++row) {
        display.fillRect(
            x + RowStarts[row] * unit,
            y + row * unit,
            (RowEnds[row] - RowStarts[row] + 1) * unit,
            unit,
            cloud);
        if (PlayStarts[row] >= 0) {
            display.fillRect(
                x + PlayStarts[row] * unit,
                y + row * unit,
                (PlayEnds[row] - PlayStarts[row] + 1) * unit,
                unit,
                play);
        }
    }
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
#elif defined(XCLOUD_NETWORK_PROBE)
    printf("[xCloud-PS4] Network probe active; sign-in and streaming are disabled.\n");
    xcloud::DisplayEngine display(1920, 1080);
    const bool displayOk = display.init();
    printf("[xCloud-PS4] Diagnostic display initialization %s.\n", displayOk ? "complete" : "failed");
    const xcloud::PixelColor background = xcloud::PixelColor::FromRGB(10, 14, 15);
    const xcloud::PixelColor green = xcloud::PixelColor::FromRGB(72, 206, 123);
    const xcloud::PixelColor white = xcloud::PixelColor::White();
    const xcloud::PixelColor muted = xcloud::PixelColor::FromRGB(153, 168, 161);
    if (displayOk) {
        display.clear(background);
        display.fillRect(0, 0, 1920, 8, green);
        display.drawText("PS4 NETWORK DIAGNOSTIC", 120, 100, 5, white);
        display.drawText("Running loopback UDP, poll, and pipe checks...", 120, 190, 2, muted);
        display.present();
    }
    std::vector<xcloud::NetworkProbeCheck> networkChecks;
    const bool networkOk = xcloud::runNetworkProbe(networkChecks);
    printf("[xCloud-PS4] POSIX network probe %s.\n", networkOk ? "passed" : "failed");
    if (displayOk) {
        display.clear(background);
        display.fillRect(0, 0, 1920, 8, networkOk ? green : xcloud::PixelColor::Yellow());
        display.drawText("PS4 NETWORK DIAGNOSTIC", 120, 100, 5, white);
        display.drawText(
            networkOk ? "RESULT: PASS" : "RESULT: FAIL",
            120,
            210,
            4,
            networkOk ? green : xcloud::PixelColor::Yellow());
        const size_t visibleChecks = std::min(networkChecks.size(), size_t{20});
        for (size_t i = 0; i < visibleChecks; ++i) {
            const auto& check = networkChecks[i];
            const std::string status = check.passed ? "PASS" : "FAIL";
            const std::string detail = check.errorCode == 0
                ? ""
                : " errno " + std::to_string(check.errorCode);
            const std::string line = check.name + ": " + status + " (rc " +
                std::to_string(check.result) + detail + ")";
            drawTextLimited(
                display,
                line,
                120,
                290 + static_cast<int>(i) * 30,
                2,
                1680,
                check.passed ? green : xcloud::PixelColor::Yellow());
        }
        display.drawText("Close the app from the PS4 menu when finished.", 120, 980, 1, muted);
        display.present();
    }
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
#elif defined(XCLOUD_WEBRTC_PROBE)
    printf("[xCloud-PS4] WebRTC probe active; xCloud sign-in and streaming are disabled.\n");
    xcloud::DisplayEngine display(1920, 1080);
    const bool displayOk = display.init();
    printf("[xCloud-PS4] Diagnostic display initialization %s.\n", displayOk ? "complete" : "failed");
    const xcloud::PixelColor background = xcloud::PixelColor::FromRGB(10, 14, 15);
    const xcloud::PixelColor green = xcloud::PixelColor::FromRGB(72, 206, 123);
    const xcloud::PixelColor white = xcloud::PixelColor::White();
    const xcloud::PixelColor muted = xcloud::PixelColor::FromRGB(153, 168, 161);
    if (displayOk) {
        display.clear(background);
        display.fillRect(0, 0, 1920, 8, green);
        display.drawText("PS4 WEBRTC TRANSPORT PROBE", 120, 100, 5, white);
        display.drawText("Creating a peer and gathering local ICE candidates...", 120, 190, 2, muted);
        display.present();
    }

    const xcloud::WebRtcProbeResult probe = xcloud::runWebRtcProbe();
    const bool probeOk = probe.peerCreated && probe.gatheringComplete &&
        probe.localDescriptionReady && probe.localCandidateCount > 0;
    printf(
        "[xCloud-PS4] WebRTC probe %s (peer=%d gather=%d sdp=%d candidates=%zu).\n",
        probeOk ? "passed" : "failed",
        probe.peerCreated,
        probe.gatheringComplete,
        probe.localDescriptionReady,
        probe.localCandidateCount);
    if (displayOk) {
        display.clear(background);
        display.fillRect(0, 0, 1920, 8, probeOk ? green : xcloud::PixelColor::Yellow());
        display.drawText("PS4 WEBRTC TRANSPORT PROBE", 120, 100, 5, white);
        display.drawText(
            probeOk ? "RESULT: PASS" : "RESULT: FAIL",
            120,
            210,
            4,
            probeOk ? green : xcloud::PixelColor::Yellow());
        display.drawText(
            "Local ICE candidates: " + std::to_string(probe.localCandidateCount),
            120,
            300,
            2,
            white);
        const std::string detail = probe.error.empty()
            ? "Peer creation, SDP generation, and ICE candidate gathering completed."
            : probe.error;
        drawTextWrapped(display, detail, 120, 360, 2, 1680, 3, muted);
        display.drawText("Close the app from the PS4 menu when finished.", 120, 980, 1, muted);
        display.present();
    }
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
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
    xcloud::StreamingPipeline pipeline;
    struct AuthUiState {
        bool codeRequested = false;
        bool loggedIn = false;
        bool cloudCatalogReady = false;
        bool sessionStarted = false;
        bool streamingActive = false;
        bool transferCodeRequested = false;
        std::string userCode;
        std::string transferCode;
        std::string gamertag;
        std::string cloudStatus;
        std::string sessionStatus;
        std::string currentStreamingTitle;
        size_t cloudTitleCount = 0;
        std::vector<xcloud::CloudTitle> cloudTitles;
        size_t selectedTitleIndex = 0;
        std::string status = XCLOUD_CLIENT_ID[0] == '\0'
            ? "Set XCLOUD_CLIENT_ID in CMake to enable sign-in."
            : "Connecting to Microsoft...";
    };
    AuthUiState authUi;
    const std::string previousStreamStage = loadLastStreamStage();
    if (!previousStreamStage.empty()) {
        authUi.sessionStatus = "Last stream checkpoint: " + previousStreamStage;
    }
    std::mutex authUiMutex;
    std::atomic<bool> stopAuth{false};
    std::thread authThread;
    std::thread sessionThread;
    xcloud::XcloudSession cloudSession;
    bool padOk = false;
    std::atomic<bool> netOk{false};
    std::atomic<bool> stopStreamingSession{false};
    bool authThreadStarted = false;
    bool crossWasDown = false;
    bool dpadUpWasDown = false;
    bool dpadDownWasDown = false;
    bool dpadLeftWasDown = false;
    bool dpadRightWasDown = false;
    bool triangleWasDown = false;
    bool showStreamHud = true;

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

            // Touchpad or Circle cancels a cloud session; Triangle toggles the stream HUD.
            bool isStreamingNow = false;
            bool isSessionStarted = false;
            {
                std::lock_guard<std::mutex> lock(authUiMutex);
                isStreamingNow = authUi.streamingActive;
                isSessionStarted = authUi.sessionStarted;
            }
            if (isSessionStarted) {
                const bool touchpadDown = (report.buttons & (1 << 7)) != 0;
                const bool circleDown = (report.buttons & (1 << 1)) != 0;
                if (touchpadDown || circleDown) {
                    printf("[xCloud-PS4] Touchpad/Circle pressed: cancelling cloud session...\n");
                    stopStreamingSession = true;
                }
            }
            if (isStreamingNow) {
                pipeline.updateGamepad(report);
                const bool triangleDown = (report.buttons & (1 << 3)) != 0;
                if (triangleDown && !triangleWasDown) {
                    showStreamHud = !showStreamHud;
                }
                triangleWasDown = triangleDown;
            }

            // D-Pad Up / Down: step 1 game
            const bool dpadUp = (report.buttons & (1 << 10)) != 0;
            if (dpadUp && !dpadUpWasDown) {
                std::lock_guard<std::mutex> lock(authUiMutex);
                if (authUi.cloudCatalogReady && !authUi.cloudTitles.empty() && !authUi.sessionStarted) {
                    if (authUi.selectedTitleIndex == 0) {
                        authUi.selectedTitleIndex = authUi.cloudTitles.size() - 1;
                    } else {
                        --authUi.selectedTitleIndex;
                    }
                }
            }
            dpadUpWasDown = dpadUp;

            const bool dpadDown = (report.buttons & (1 << 11)) != 0;
            if (dpadDown && !dpadDownWasDown) {
                std::lock_guard<std::mutex> lock(authUiMutex);
                if (authUi.cloudCatalogReady && !authUi.cloudTitles.empty() && !authUi.sessionStarted) {
                    authUi.selectedTitleIndex = (authUi.selectedTitleIndex + 1) % authUi.cloudTitles.size();
                }
            }
            dpadDownWasDown = dpadDown;

            // D-Pad Left / Right: jump 10 games
            const bool dpadLeft = (report.buttons & (1 << 12)) != 0;
            if (dpadLeft && !dpadLeftWasDown) {
                std::lock_guard<std::mutex> lock(authUiMutex);
                if (authUi.cloudCatalogReady && !authUi.cloudTitles.empty() && !authUi.sessionStarted) {
                    authUi.selectedTitleIndex = (authUi.selectedTitleIndex >= 10)
                        ? authUi.selectedTitleIndex - 10
                        : 0;
                }
            }
            dpadLeftWasDown = dpadLeft;

            const bool dpadRight = (report.buttons & (1 << 13)) != 0;
            if (dpadRight && !dpadRightWasDown) {
                std::lock_guard<std::mutex> lock(authUiMutex);
                if (authUi.cloudCatalogReady && !authUi.cloudTitles.empty() && !authUi.sessionStarted) {
                    authUi.selectedTitleIndex = std::min(
                        authUi.selectedTitleIndex + 10,
                        authUi.cloudTitles.size() - 1);
                }
            }
            dpadRightWasDown = dpadRight;

            // Cross: transmit streaming for the currently selected detected game
            const bool crossDown = (report.buttons & (1 << 0)) != 0;
            if (crossDown && !crossWasDown) {
                std::string titleId;
                std::string titleName;
                {
                    std::lock_guard<std::mutex> lock(authUiMutex);
                    if (authUi.cloudCatalogReady && !authUi.cloudTitles.empty() && !authUi.sessionStarted) {
                        authUi.sessionStarted = true;
                        authUi.streamingActive = false;
                        stopStreamingSession = false;
                        const auto& sel = authUi.cloudTitles[authUi.selectedTitleIndex];
                        titleId = sel.titleId;
                        titleName = sel.titleName.empty() ? (sel.productId.empty() ? sel.titleId : sel.productId) : sel.titleName;
                        authUi.currentStreamingTitle = titleName;
                        authUi.sessionStatus = "Connecting to " + titleName + "...";
                        pipeline.setStreamInfo(titleName);
                    }
                }
                if (!titleId.empty()) {
                    saveLastStreamStage("Stream attempt queued for " + titleName);
                    if (sessionThread.joinable()) {
                        sessionThread.join();
                    }
                    sessionThread = std::thread([&, titleId, titleName]() {
                        try {
                            auto updateStatus = [&](const std::string& status) {
                                saveLastStreamStage(status);
                                std::lock_guard<std::mutex> lock(authUiMutex);
                                authUi.sessionStatus = status;
                            };

                            std::string error;
                            if (!cloudSession.startSession(titleId, error)) {
                                if (error.find("500") != std::string::npos || error.find("internal server error") != std::string::npos) {
                                    updateStatus("HTTP 500: Account lacks Game Pass streaming license for this game. Select an entitled game (e.g. Fortnite)!");
                                } else {
                                    updateStatus("Session start failed: " + error);
                                }
                                pipeline.stop();
                                {
                                    std::lock_guard<std::mutex> lock(authUiMutex);
                                    authUi.sessionStarted = false;
                                    authUi.streamingActive = false;
                                }
                                return;
                            }

                            bool sessionHandoffSent = false;
                            const auto startTime = std::chrono::steady_clock::now();
                            const auto deadline = startTime + std::chrono::minutes(10);
                            while (!stopAuth.load() && !stopStreamingSession.load() &&
                                   std::chrono::steady_clock::now() < deadline) {
                                const auto elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::steady_clock::now() - startTime).count();
                                std::string state;
                                if (!cloudSession.getSessionState(state, error)) {
                                    updateStatus(error);
                                    pipeline.stop();
                                    cloudSession.stopSession();
                                    {
                                        std::lock_guard<std::mutex> lock(authUiMutex);
                                        authUi.sessionStarted = false;
                                        authUi.streamingActive = false;
                                    }
                                    return;
                                }
                                if (state == "WaitingForResources") {
                                    updateStatus(titleName + " in cloud queue (" + std::to_string(elapsedSec) + "s elapsed)...");
                                } else if (state == "Provisioning") {
                                    updateStatus("Provisioning " + titleName + " (" + std::to_string(elapsedSec) + "s elapsed)...");
                                } else if (state == "ReadyToConnect") {
                                    if (!sessionHandoffSent) {
                                        updateStatus("Acquiring cloud streaming transfer token...");
                                        std::string transferToken;
                                        if (!auth.acquireConsoleTransferToken(transferToken)) {
                                            updateStatus("Transfer token failed: " + auth.getLastError());
                                            pipeline.stop();
                                            cloudSession.stopSession();
                                            {
                                                std::lock_guard<std::mutex> lock(authUiMutex);
                                                authUi.sessionStarted = false;
                                                authUi.streamingActive = false;
                                            }
                                            return;
                                        }

                                        updateStatus("Connecting cloud stream session...");
                                        if (!cloudSession.connectSession(transferToken, error)) {
                                            updateStatus("Connect session failed: " + error);
                                            pipeline.stop();
                                            cloudSession.stopSession();
                                            {
                                                std::lock_guard<std::mutex> lock(authUiMutex);
                                                authUi.sessionStarted = false;
                                                authUi.streamingActive = false;
                                            }
                                            return;
                                        }
                                        sessionHandoffSent = true;
                                        updateStatus("Handoff accepted. Awaiting blade Provisioned state...");
                                    } else {
                                        updateStatus("Blade starting up; awaiting Provisioned state...");
                                    }
                                } else if (state == "Provisioned") {
                                    updateStatus("Creating WebRTC video, audio, and controller channels...");
                                    std::string localOffer;
                                    if (!pipeline.createWebRtcOffer(localOffer, error)) {
                                        updateStatus("WebRTC setup failed: " + error);
                                        pipeline.stop();
                                        cloudSession.stopSession();
                                        std::lock_guard<std::mutex> lock(authUiMutex);
                                        authUi.sessionStarted = false;
                                        authUi.streamingActive = false;
                                        return;
                                    }

                                    updateStatus("Sending real WebRTC offer to the cloud session...");
                                    std::string remoteAnswer;
                                    if (!cloudSession.exchangeSdp(localOffer, remoteAnswer, error, updateStatus)) {
                                        updateStatus("Cloud SDP negotiation failed: " + error);
                                        pipeline.stop();
                                        cloudSession.stopSession();
                                        std::lock_guard<std::mutex> lock(authUiMutex);
                                        authUi.sessionStarted = false;
                                        authUi.streamingActive = false;
                                        return;
                                    }
                                    if (!pipeline.acceptWebRtcAnswer(remoteAnswer, error)) {
                                        updateStatus("Cloud returned an invalid WebRTC answer: " + error);
                                        pipeline.stop();
                                        cloudSession.stopSession();
                                        std::lock_guard<std::mutex> lock(authUiMutex);
                                        authUi.sessionStarted = false;
                                        authUi.streamingActive = false;
                                        return;
                                    }

                                    updateStatus("Exchanging ICE candidates with the cloud session...");
                                    const auto localCandidates = pipeline.getLocalIceCandidates();
                                    std::vector<xcloud::WebRtcSignaledCandidate> remoteCandidates;
                                    if (!cloudSession.exchangeIceCandidates(localCandidates, remoteCandidates, error, updateStatus)) {
                                        updateStatus("WebRTC ICE exchange failed: " + error);
                                        pipeline.stop();
                                        cloudSession.stopSession();
                                        std::lock_guard<std::mutex> lock(authUiMutex);
                                        authUi.sessionStarted = false;
                                        authUi.streamingActive = false;
                                        return;
                                    }
                                    for (const auto& candidate : remoteCandidates) {
                                        if (!pipeline.addRemoteIceCandidate(candidate, error)) {
                                            updateStatus("Could not add cloud ICE candidate: " + error);
                                            pipeline.stop();
                                            cloudSession.stopSession();
                                            std::lock_guard<std::mutex> lock(authUiMutex);
                                            authUi.sessionStarted = false;
                                            authUi.streamingActive = false;
                                            return;
                                        }
                                    }

                                    updateStatus("Checking ICE connectivity to the xCloud blade...");
                                    if (!pipeline.waitForWebRtcConnection(30000, error)) {
                                        updateStatus("WebRTC connection failed: " + error);
                                        pipeline.stop();
                                        cloudSession.stopSession();
                                        std::lock_guard<std::mutex> lock(authUiMutex);
                                        authUi.sessionStarted = false;
                                        authUi.streamingActive = false;
                                        return;
                                    }

                                    if (!pipeline.startStreaming({}, error)) {
                                        updateStatus("WebRTC connected, but media could not start: " + error);
                                        pipeline.stop();
                                        cloudSession.stopSession();
                                        std::lock_guard<std::mutex> lock(authUiMutex);
                                        authUi.sessionStarted = false;
                                        authUi.streamingActive = false;
                                        return;
                                    }

                                    updateStatus("WebRTC connected. PS4 hardware H.264 decoder is active; audio decoding is not implemented yet.");
                                    {
                                        std::lock_guard<std::mutex> lock(authUiMutex);
                                        authUi.streamingActive = true;
                                        authUi.sessionStatus = "Video is being decoded by the PS4 hardware AVC decoder. Audio is not implemented yet.";
                                    }
                                    while (!stopAuth.load() && !stopStreamingSession.load()) {
                                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                                    }
                                    pipeline.stop();
                                    cloudSession.stopSession();
                                    {
                                        std::lock_guard<std::mutex> lock(authUiMutex);
                                        authUi.sessionStarted = false;
                                        authUi.streamingActive = false;
                                        authUi.sessionStatus = "Cloud session stopped.";
                                    }
                                    return;
                                } else {
                                    updateStatus("Cloud session state: " + state);
                                }
                                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                            }

                            if (stopStreamingSession.load()) {
                                updateStatus("Cloud session cancelled.");
                            } else if (!stopAuth.load()) {
                                updateStatus("Cloud session timed out; try again later.");
                            }
                            pipeline.stop();
                            cloudSession.stopSession();
                            {
                                std::lock_guard<std::mutex> lock(authUiMutex);
                                authUi.sessionStarted = false;
                                authUi.streamingActive = false;
                            }
                        } catch (const std::exception& e) {
                            printf("[xCloud-PS4] Exception in session thread: %s\n", e.what());
                        } catch (...) {
                            printf("[xCloud-PS4] Unknown exception in session thread.\n");
                        }
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

        if (authSnapshot.streamingActive) {
            const uint8_t* frameRgba = nullptr;
            uint32_t frameW = 0, frameH = 0;
            if (pipeline.getLatestFrame(frameRgba, frameW, frameH) && frameRgba != nullptr && frameW > 0 && frameH > 0) {
                std::string drawErr;
                display.drawRgbaFrame(frameRgba, frameW, frameH, frameW * 4, drawErr);
            } else {
                display.clear(xcloud::PixelColor::FromRGB(6, 10, 10));

                // Ambient glowing stream canvas borders
                display.drawBorder(0, 0, 1920, 1080, 4, greenDim);
                display.drawBorder(4, 4, 1912, 1072, 1, line);

                // Stream Center Card
                const int cardX = 460;
                const int cardY = 280;
                const int cardW = 1000;
                const int cardH = 490;
                display.fillRect(cardX, cardY, cardW, cardH, raised);
                display.drawBorder(cardX, cardY, cardW, cardH, 2, green);

                drawBadge(display, cardX + 50, cardY + 45, 330, "WEBRTC CONNECTED", green, greenDim);
                drawTextLimited(display, authSnapshot.currentStreamingTitle, cardX + 50, cardY + 115, 4, 900, white);
                display.drawText("Encrypted media transport connected", cardX + 50, cardY + 180, 2, green);
                display.fillRect(cardX + 50, cardY + 225, 900, 1, line);

                // Dynamic radar pulse animation
                static uint32_t pulseTick = 0;
                ++pulseTick;
                const int pulsePhase = (pulseTick / 12) % 4;
                std::string pulseDots;
                for (int p = 0; p <= pulsePhase; ++p) pulseDots += ".";

                // Live Stream Telemetry
                char statsBuf[256];
                snprintf(statsBuf, sizeof(statsBuf), "Video packets sent: %llu   |   WebRTC: %s",
                         static_cast<unsigned long long>(pipeline.getFramesTransmitted()),
                         pipeline.getStateString().c_str());
                display.drawText(statsBuf, cardX + 50, cardY + 255, 1, muted);

                display.drawText("Controller input channel is active.", cardX + 50, cardY + 295, 2, white);
                const std::string awaitingMsg = "Waiting for the first decoded video frame" + pulseDots;
                display.drawText(awaitingMsg, cardX + 50, cardY + 340, 2, gold);

                display.fillRect(cardX + 50, cardY + 395, 900, 52, greenDim);
                display.drawBorder(cardX + 50, cardY + 395, 900, 52, 1, green);
                display.drawText("TOUCHPAD / CIRCLE: EXIT STREAM   |   TRIANGLE: TOGGLE HUD", cardX + 130, cardY + 413, 1, white);
            }

            // Top Stream Floating HUD (toggleable with Triangle)
            if (showStreamHud) {
                display.fillRect(40, 25, 520, 38, xcloud::PixelColor::FromRGB(12, 16, 16));
                display.drawBorder(40, 25, 520, 38, 1, greenDim);
                display.drawText("WEBRTC", 60, 36, 1, green);
                display.drawText("|", 110, 36, 1, line);
                drawTextLimited(display, authSnapshot.currentStreamingTitle, 130, 36, 1, 410, white);

                display.fillRect(1420, 25, 460, 38, xcloud::PixelColor::FromRGB(12, 16, 16));
                display.drawBorder(1420, 25, 460, 38, 1, greenDim);
                display.drawText("1080p 60FPS | H.264 | SceAudioOut", 1440, 36, 1, muted);
            }
        } else {
            display.clear(background);

            display.fillRect(0, 0, 1920, 5, green);
            display.fillRect(0, 5, 1920, 3, greenDim);
            drawCloudMark(display, 112, 36, 6, green, background);
            display.drawText("XCLOUD STREAM", 188, 38, 4, white);
        display.drawText("GAME STREAMING CLIENT", 190, 78, 1, muted);
        drawBadge(
            display,
            1530,
            43,
            280,
            std::string("PS4 HOMEBREW  ") + AppBuildVersion,
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
            display.drawText("GAME SELECTION", 1210, 225, 1, muted);
            if (authSnapshot.cloudCatalogReady) {
                if (!authSnapshot.cloudTitles.empty()) {
                    const size_t idx = std::min(authSnapshot.selectedTitleIndex, authSnapshot.cloudTitles.size() - 1);
                    const auto& selected = authSnapshot.cloudTitles[idx];
                    const std::string titleDisplay = selected.titleName.empty()
                        ? (selected.productId.empty() ? selected.titleId : selected.productId)
                        : selected.titleName;

                    const bool isPlayable = selected.entitled || selected.freeInStore || selected.productId == "BT5P2X999VH2";

                    if (authSnapshot.streamingActive) {
                        drawBadge(display, 1210, 275, 290, "WEBRTC CONNECTED", green, greenDim);
                        drawTextLimited(display, authSnapshot.currentStreamingTitle, 1210, 340, 2, 510, white);
                        display.drawText("PS4 video active; audio pending", 1210, 380, 1, gold);
                    } else if (isPlayable) {
                        drawBadge(display, 1210, 275, 270, "PLAYABLE (ENTITLED)", green, greenDim);
                        drawTextLimited(display, titleDisplay, 1210, 340, 2, 510, white);
                        const std::string indexLabel = "[" + std::to_string(idx + 1) + "/" +
                            std::to_string(authSnapshot.cloudTitleCount) + "]  DPAD: BROWSE GAMES";
                        display.drawText(indexLabel.c_str(), 1210, 380, 1, muted);
                    } else {
                        drawBadge(display, 1210, 275, 270, "GAME PASS REQUIRED", gold, xcloud::PixelColor::FromRGB(46, 37, 22));
                        drawTextLimited(display, titleDisplay, 1210, 340, 2, 510, white);
                        const std::string indexLabel = "[" + std::to_string(idx + 1) + "/" +
                            std::to_string(authSnapshot.cloudTitleCount) + "]  NOT IN SUBSCRIPTION";
                        display.drawText(indexLabel.c_str(), 1210, 380, 1, gold);
                    }

                    if (!authSnapshot.sessionStatus.empty()) {
                        display.fillRect(1210, 420, 510, 90, background);
                        display.drawBorder(1210, 420, 510, 90, 1, line);
                        drawTextWrapped(display, authSnapshot.sessionStatus, 1230, 433, 1, 470, 4, muted);
                    }

                    if (!authSnapshot.streamingActive) {
                        display.fillRect(1210, 530, 510, 48, isPlayable ? greenDim : xcloud::PixelColor::FromRGB(35, 30, 20));
                        display.drawBorder(1210, 530, 510, 48, 1, isPlayable ? green : gold);
                        display.drawText("PRESS CROSS: TRANSMIT STREAM", 1230, 545, 1, isPlayable ? white : gold);
                    }
                } else {
                    drawBadge(display, 1210, 280, 244, "CATALOG LOADED", green, greenDim);
                    display.drawText("NO TITLES RETURNED", 1210, 350, 2, white);
                    display.drawText("Check subscription or regional availability.", 1210, 390, 1, muted);
                }
            } else if (!authSnapshot.cloudStatus.empty()) {
                drawBadge(display, 1210, 280, 244, "CLOUD LOGIN ISSUE", gold, xcloud::PixelColor::FromRGB(46, 37, 22));
                drawTextLimited(display, authSnapshot.cloudStatus, 1210, 350, 1, 510, muted);
            } else {
                display.drawText("STREAMING IS", 1210, 300, 3, white);
                display.drawText("NOT AVAILABLE YET", 1210, 345, 3, gold);
            }
            display.fillRect(1210, 600, 510, 1, line);
            if (!authSnapshot.cloudCatalogReady && authSnapshot.cloudStatus.empty()) {
                display.drawText("Game-session setup and", 1210, 620, 1, muted);
                display.drawText("video decoding are still", 1210, 642, 1, muted);
                display.drawText("being developed.", 1210, 664, 1, muted);
            }
        }

        display.fillRect(110, 935, 1700, 1, line);
        drawBadge(
            display,
            110,
            955,
            240,
            padOk ? "CONTROLLER READY" : "CONTROLLER OFFLINE",
            padOk ? green : gold,
            padOk ? greenDim : xcloud::PixelColor::FromRGB(46, 37, 22));
        drawBadge(
            display,
            365,
            955,
            200,
            netOk.load() ? "NETWORK READY" : "NETWORK IDLE",
            netOk.load() ? green : muted,
            netOk.load() ? greenDim : raised);
        drawBadge(
            display,
            580,
            955,
            260,
            pipeline.isTransmitting() ? "PIPELINE: TRANSMITTING" : "PIPELINE: READY",
            pipeline.isTransmitting() ? green : muted,
            pipeline.isTransmitting() ? greenDim : raised);
        drawTextLimited(display, "OPTIONS + TOUCHPAD  EXIT", 1500, 966, 1, 300, muted);
        }

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

                // 1. Check if a saved session exists on disk
                bool loggedInWithSaved = false;
                {
                    std::lock_guard<std::mutex> lock(authUiMutex);
                    authUi.status = "Checking for saved login...";
                }
                if (auth.loadSavedTokens()) {
                    printf("[xCloud-PS4] Successfully logged in using saved session (Gamertag: %s)!\n",
                           auth.getTokens().gamertag.c_str());
                    {
                        std::lock_guard<std::mutex> lock(authUiMutex);
                        authUi.gamertag = auth.getTokens().gamertag;
                        authUi.loggedIn = true;
                        authUi.status = "Welcome back, " + auth.getTokens().gamertag;
                        authUi.cloudStatus = "Connecting to cloud gaming...";
                    }
                    loggedInWithSaved = true;
                }

                if (!loggedInWithSaved) {
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

                    // Save session tokens persistently
                    auth.saveTokens();

                    {
                        std::lock_guard<std::mutex> lock(authUiMutex);
                        authUi.gamertag = auth.getTokens().gamertag;
                        authUi.loggedIn = true;
                        authUi.status = "Xbox account authenticated.";
                        authUi.cloudStatus = "Connecting to cloud gaming...";
                    }
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

                std::lock_guard<std::mutex> lock(authUiMutex);
                authUi.cloudTitleCount = cloudTitles.size();
                authUi.cloudCatalogReady = true;
                authUi.cloudTitles = std::move(cloudTitles);
                authUi.selectedTitleIndex = 0;
                authUi.cloudStatus = "Catalog loaded: " + std::to_string(authUi.cloudTitleCount) + " games detected.";
                });
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 FPS
    }

    // Cleanup
    stopAuth = true;
    if (sessionThread.joinable()) sessionThread.join();
    if (authThread.joinable()) authThread.join();
    pipeline.shutdown();
    display.shutdown();
    pad.shutdown();
    decoder.shutdown();
    if (netOk.load()) xcloud::HttpClient::shutdown();

    printf("[xCloud-PS4] Exited cleanly.\n");
    return 0;
#endif
}
