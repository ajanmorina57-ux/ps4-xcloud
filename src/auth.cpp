#include "auth.hpp"
#include "http_client.hpp"
#include "json.hpp"

#include <cstdio>
#include <chrono>
#include <thread>
#include <algorithm>
#include <sys/stat.h>

using json = nlohmann::json;

namespace xcloud {

static std::string formEncode(const std::string& value) {
    static constexpr char Hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());
    for (const unsigned char c : value) {
        const bool unreserved = (c >= 'A' && c <= 'Z') ||
                                (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') ||
                                c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            encoded.push_back(static_cast<char>(c));
        } else {
            encoded.push_back('%');
            encoded.push_back(Hex[c >> 4]);
            encoded.push_back(Hex[c & 0x0F]);
        }
    }
    return encoded;
}

AuthManager::AuthManager(const std::string& clientId)
    : m_clientId(clientId) {}

bool AuthManager::requestDeviceCode(DeviceCodeResponse& outResponse) {
    const std::string url = "https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode";
    const std::string body = "client_id=" + formEncode(m_clientId) +
        "&scope=" + formEncode("xboxlive.signin openid profile offline_access");

    HttpResponse resp = HttpClient::post(url, body, "application/x-www-form-urlencoded");
    if (!resp.success) {
        m_lastError = "Failed to request device code. HTTP " + std::to_string(resp.statusCode) + ": " + resp.body;
        return false;
    }

    try {
        auto j = json::parse(resp.body);
        if (j.contains("error")) {
            m_lastError = j.value("error_description", j.value("error", "Unknown Microsoft error"));
            return false;
        }

        outResponse.device_code = j["device_code"].get<std::string>();
        outResponse.user_code = j["user_code"].get<std::string>();
        outResponse.verification_uri = j.value("verification_uri", "https://microsoft.com/link");
        outResponse.interval = j.value("interval", 5);
        outResponse.expires_in = j.value("expires_in", 900);
        return true;
    } catch (const std::exception& e) {
        m_lastError = std::string("JSON error on device code: ") + e.what();
        return false;
    }
}

bool AuthManager::pollForTokens(
    const std::string& deviceCode,
    int intervalSec,
    int timeoutSec,
    const std::atomic<bool>* stopRequested) {
    const std::string url = "https://login.microsoftonline.com/consumers/oauth2/v2.0/token";
    std::string body = "grant_type=urn:ietf:params:oauth:grant-type:device_code"
                       "&client_id=" + formEncode(m_clientId) +
                       "&device_code=" + formEncode(deviceCode);

    auto startTime = std::chrono::steady_clock::now();
    int sleepSec = intervalSec > 0 ? intervalSec : 5;

    while (true) {
        if (stopRequested && stopRequested->load()) return false;

        auto now = std::chrono::steady_clock::now();
        int elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count();
        if (elapsed > timeoutSec) {
            m_lastError = "Sign-in timed out.";
            return false;
        }

        for (int remainingMs = sleepSec * 1000; remainingMs > 0; remainingMs -= 100) {
            if (stopRequested && stopRequested->load()) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min(remainingMs, 100)));
        }

        HttpResponse resp = HttpClient::post(url, body, "application/x-www-form-urlencoded");
        if (resp.success) {
            try {
                auto j = json::parse(resp.body);
                if (j.contains("access_token")) {
                    m_accessToken = j["access_token"].get<std::string>();
                    m_refreshToken = j.value("refresh_token", "");
                    return true;
                }
            } catch (...) {}
        } else {
            try {
                auto j = json::parse(resp.body);
                std::string err = j.value("error", "");
                if (err == "authorization_pending") {
                    continue;
                } else if (err == "slow_down") {
                    sleepSec += 5;
                    continue;
                } else {
                    m_lastError = j.value("error_description", err);
                    return false;
                }
            } catch (...) {}
        }
    }
}

bool AuthManager::acquireConsoleTransferToken(std::string& transferToken) {
    transferToken.clear();
    if (m_refreshToken.empty()) {
        m_lastError = "No refresh token available to acquire streaming transfer token.";
        return false;
    }

    const std::string url = "https://login.live.com/oauth20_token.srf";
    const std::string scope = "service::http://Passport.NET/purpose::PURPOSE_XBOX_CLOUD_CONSOLE_TRANSFER_TOKEN";
    const std::string body =
        "grant_type=refresh_token"
        "&client_id=" + formEncode(m_clientId) +
        "&refresh_token=" + formEncode(m_refreshToken) +
        "&scope=" + formEncode(scope);

    const HttpResponse resp = HttpClient::post(url, body, "application/x-www-form-urlencoded");
    if (!resp.success) {
        m_lastError = "Transfer token request failed (HTTP " + std::to_string(resp.statusCode) + "): " + resp.body;
        return false;
    }

    try {
        const auto j = json::parse(resp.body);
        transferToken = j.value("lpt", j.value("access_token", std::string()));
        if (transferToken.empty()) {
            m_lastError = "Microsoft returned no transfer token (LPT) in response.";
            return false;
        }
        printf("[Auth] Successfully acquired xCloud streaming transfer token (LPT)!\n");
        return true;
    } catch (const std::exception& e) {
        m_lastError = std::string("Failed to parse transfer token response: ") + e.what();
        return false;
    }
}

// Fallback compatibility methods
bool AuthManager::requestConsoleTransferCode(DeviceCodeResponse& outResponse) {
    return requestDeviceCode(outResponse);
}

bool AuthManager::pollForConsoleTransferToken(
    const std::string&,
    int,
    int,
    std::string& transferToken,
    const std::atomic<bool>*) {
    return acquireConsoleTransferToken(transferToken);
}

bool AuthManager::authenticateXboxLive() {
    if (m_accessToken.empty()) {
        m_lastError = "No Microsoft access token available.";
        return false;
    }

    // 1. Authenticate with Xbox Live User service
    std::string userAuthUrl = "https://user.auth.xboxlive.com/user/authenticate";
    auto makeUserAuthRequest = [this](const std::string& ticketPrefix) {
        return json{
            {"RelyingParty", "http://auth.xboxlive.com"},
            {"TokenType", "JWT"},
            {"Properties", {
                {"AuthMethod", "RPS"},
                {"SiteName", "user.auth.xboxlive.com"},
                {"RpsTicket", ticketPrefix + m_accessToken}
            }}
        };
    };

    // login.live.com's Xbox MSA flow normally returns an RPS ticket accepted
    // with the `t=` prefix. Keep `d=` as a compatibility fallback.
    HttpResponse userResp = HttpClient::post(userAuthUrl, makeUserAuthRequest("t=").dump());
    if (!userResp.success) {
        userResp = HttpClient::post(userAuthUrl, makeUserAuthRequest("d=").dump());
    }
    if (!userResp.success) {
        m_lastError = "Xbox User authentication failed (HTTP " + std::to_string(userResp.statusCode) + "): " + userResp.body;
        return false;
    }

    try {
        auto j = json::parse(userResp.body);
        m_tokens.user_token = j["Token"].get<std::string>();
        m_tokens.user_hash = j["DisplayClaims"]["xui"][0]["uhs"].get<std::string>();
    } catch (const std::exception& e) {
        m_lastError = std::string("Failed to parse Xbox user token: ") + e.what();
        return false;
    }

    // 2. Request XSTS Token for xCloud (Game Streaming)
    std::string xstsUrl = "https://xsts.auth.xboxlive.com/xsts/authorize";
    json xstsReq = {
        {"RelyingParty", "http://gssv.xboxlive.com/"},
        {"TokenType", "JWT"},
        {"Properties", {
            {"UserTokens", {m_tokens.user_token}},
            {"SandboxId", "RETAIL"}
        }}
    };

    HttpResponse xstsResp = HttpClient::post(xstsUrl, xstsReq.dump());
    if (!xstsResp.success) {
        m_lastError = "XSTS authentication failed (HTTP " + std::to_string(xstsResp.statusCode) + "): " + xstsResp.body;
        return false;
    }

    try {
        auto j = json::parse(xstsResp.body);
        m_tokens.xsts_token = j["Token"].get<std::string>();
        if (j["DisplayClaims"]["xui"][0].contains("gtg")) {
            m_tokens.gamertag = j["DisplayClaims"]["xui"][0]["gtg"].get<std::string>();
        }
        return true;
    } catch (const std::exception& e) {
        m_lastError = std::string("Failed to parse XSTS token: ") + e.what();
        return false;
    }
}

static std::vector<std::string> getCandidateTokenPaths() {
    std::vector<std::string> paths;
#if defined(__ORBIS__) || defined(__PS4__)
    // PS4 persistent homebrew storage partition
    mkdir("/data", 0777);
    mkdir("/data/ps4-xcloud", 0777);
    paths.push_back("/data/ps4-xcloud/tokens.json");
    paths.push_back("/data/tokens.json");
#endif
    const char* home = getenv("HOME");
    if (home) {
        paths.push_back(std::string(home) + "/.config/ps4-xcloud/tokens.json");
        paths.push_back(std::string(home) + "/tokens.json");
    }
    paths.push_back("tokens.json");
    return paths;
}

bool AuthManager::saveTokens() const {
    if (m_refreshToken.empty() && m_tokens.xsts_token.empty()) {
        return false;
    }
    json j;
    j["clientId"] = m_clientId;
    j["refreshToken"] = m_refreshToken;
    j["accessToken"] = m_accessToken;
    j["userToken"] = m_tokens.user_token;
    j["xstsToken"] = m_tokens.xsts_token;
    j["userHash"] = m_tokens.user_hash;
    j["gamertag"] = m_tokens.gamertag;

    const std::string serialized = j.dump(2);
    const auto paths = getCandidateTokenPaths();
    for (const auto& path : paths) {
        FILE* fp = fopen(path.c_str(), "w");
        if (fp) {
            fwrite(serialized.data(), 1, serialized.size(), fp);
            fclose(fp);
            printf("[Auth] Successfully saved session tokens to %s\n", path.c_str());
            return true;
        }
    }
    return false;
}

bool AuthManager::loadSavedTokens() {
    const auto paths = getCandidateTokenPaths();
    for (const auto& path : paths) {
        FILE* fp = fopen(path.c_str(), "r");
        if (!fp) continue;

        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (sz <= 0 || sz > 1024 * 1024) {
            fclose(fp);
            continue;
        }

        std::string content(sz, '\0');
        size_t readBytes = fread(&content[0], 1, sz, fp);
        fclose(fp);
        if (readBytes == 0) continue;

        try {
            auto j = json::parse(content);
            if (j.value("clientId", std::string()) != m_clientId) {
                printf("[Auth] Saved login was created for a different Microsoft client; requesting sign-in again.\n");
                continue;
            }
            m_refreshToken = j.value("refreshToken", "");
            m_accessToken = j.value("accessToken", "");
            m_tokens.user_token = j.value("userToken", "");
            m_tokens.xsts_token = j.value("xstsToken", "");
            m_tokens.user_hash = j.value("userHash", "");
            m_tokens.gamertag = j.value("gamertag", "");

            if (!m_refreshToken.empty()) {
                printf("[Auth] Found saved login in %s (Gamertag: %s). Refreshing token...\n",
                       path.c_str(), m_tokens.gamertag.c_str());
                if (refreshTokens()) {
                    printf("[Auth] Saved session successfully refreshed!\n");
                    return true;
                }
            }

            if (!m_tokens.xsts_token.empty() && !m_tokens.user_hash.empty()) {
                printf("[Auth] Using existing saved XSTS token for Gamertag: %s\n", m_tokens.gamertag.c_str());
                return true;
            }
        } catch (const std::exception& e) {
            printf("[Auth] Failed to parse %s: %s\n", path.c_str(), e.what());
        }
    }
    return false;
}

bool AuthManager::refreshTokens() {
    if (m_refreshToken.empty()) {
        m_lastError = "No refresh token available.";
        return false;
    }
    const std::string url = "https://login.microsoftonline.com/consumers/oauth2/v2.0/token";
    const std::string body =
        "grant_type=refresh_token"
        "&client_id=" + formEncode(m_clientId) +
        "&refresh_token=" + formEncode(m_refreshToken) +
        "&scope=" + formEncode("xboxlive.signin openid profile offline_access");

    const HttpResponse resp = HttpClient::post(url, body, "application/x-www-form-urlencoded");
    if (!resp.success) {
        m_lastError = "Token refresh failed: " + resp.body;
        return false;
    }

    try {
        const auto j = json::parse(resp.body);
        if (j.contains("access_token")) {
            m_accessToken = j["access_token"].get<std::string>();
            if (j.contains("refresh_token")) {
                m_refreshToken = j["refresh_token"].get<std::string>();
            }
            if (authenticateXboxLive()) {
                saveTokens();
                return true;
            }
        }
        return false;
    } catch (const std::exception& e) {
        m_lastError = std::string("Failed to parse refreshed token response: ") + e.what();
        return false;
    }
}

} // namespace xcloud
