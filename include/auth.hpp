#pragma once
#include <atomic>
#include <string>
#include <functional>

namespace xcloud {

struct DeviceCodeResponse {
    std::string device_code;
    std::string user_code;
    std::string verification_uri;
    int expires_in = 0;
    int interval = 5;
};

struct XboxTokens {
    std::string user_token;
    std::string xsts_token;
    std::string user_hash;
    std::string gamertag;
};

class AuthManager {
public:
    AuthManager(const std::string& clientId);

    // Starts the OAuth2 device code flow (shows code on TV screen)
    bool requestDeviceCode(DeviceCodeResponse& outResponse);
    bool requestConsoleTransferCode(DeviceCodeResponse& outResponse);
    const std::string& getLastError() const { return m_lastError; }

    // Polls Microsoft identity endpoint until the user confirms on phone/PC
    bool pollForTokens(
        const std::string& deviceCode,
        int intervalSec,
        int timeoutSec,
        const std::atomic<bool>* stopRequested = nullptr);
    bool pollForConsoleTransferToken(
        const std::string& deviceCode,
        int intervalSec,
        int timeoutSec,
        std::string& transferToken,
        const std::atomic<bool>* stopRequested = nullptr);

    // Exchanges OAuth token for Xbox Live User Token and XSTS Token
    bool authenticateXboxLive();

    const XboxTokens& getTokens() const { return m_tokens; }

private:
    std::string m_clientId;
    std::string m_lastError;
    std::string m_accessToken;
    std::string m_refreshToken;
    XboxTokens m_tokens;
};

} // namespace xcloud
