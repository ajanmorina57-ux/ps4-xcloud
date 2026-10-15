#include "auth.hpp"
#include "http_client.hpp"
#include "json.hpp"

#include <algorithm>
#include <cstdio>
#include <chrono>
#include <thread>

using json = nlohmann::json;

namespace xcloud {
namespace {

std::string formEncode(const std::string& value) {
    static constexpr char Hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (const unsigned char character : value) {
        const bool unreserved = (character >= 'A' && character <= 'Z') ||
                                (character >= 'a' && character <= 'z') ||
                                (character >= '0' && character <= '9') ||
                                character == '-' || character == '_' ||
                                character == '.' || character == '~';
        if (unreserved) {
            encoded.push_back(static_cast<char>(character));
        } else {
            encoded.push_back('%');
            encoded.push_back(Hex[character >> 4]);
            encoded.push_back(Hex[character & 0x0F]);
        }
    }
    return encoded;
}

} // namespace

AuthManager::AuthManager(const std::string& clientId)
    : m_clientId(clientId) {}

bool AuthManager::requestDeviceCode(DeviceCodeResponse& outResponse) {
    std::string url = "https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode";
    std::string body = "client_id=" + m_clientId +
                       "&scope=XboxLive.signin%20offline_access";

    HttpResponse resp = HttpClient::post(url, body, "application/x-www-form-urlencoded");
    if (!resp.success) {
        printf("[Auth] Failed to request device code. HTTP %d: %s\n", resp.statusCode, resp.body.c_str());
        m_lastError = resp.error;
        try {
            auto j = json::parse(resp.body);
            m_lastError = j.value("error_description", j.value("error", m_lastError));
        } catch (const std::exception&) {
            if (!resp.body.empty()) m_lastError = resp.body;
        }
        if (m_lastError.empty()) {
            m_lastError = resp.statusCode > 0
                ? "Microsoft returned HTTP " + std::to_string(resp.statusCode) + "."
                : "Network request failed. Check internet connection and DNS.";
        }
        return false;
    }

    try {
        auto j = json::parse(resp.body);
        outResponse.device_code = j["device_code"].get<std::string>();
        outResponse.user_code = j["user_code"].get<std::string>();
        outResponse.verification_uri = j["verification_uri"].get<std::string>();
        outResponse.interval = j.value("interval", 5);
        outResponse.expires_in = j.value("expires_in", 900);
        return true;
    } catch (const std::exception& e) {
        printf("[Auth] JSON parse error on device code: %s\n", e.what());
        m_lastError = "Microsoft returned an invalid sign-in response.";
        return false;
    }
}

bool AuthManager::requestConsoleTransferCode(DeviceCodeResponse& outResponse) {
    const std::string url = "https://login.live.com/oauth20_connect.srf";
    const std::string scope =
        "service::http://Passport.NET/purpose::PURPOSE_XBOX_CLOUD_CONSOLE_TRANSFER_TOKEN";
    const std::string body =
        "client_id=" + formEncode(m_clientId) +
        "&scope=" + formEncode(scope) +
        "&response_type=device_code";

    const HttpResponse response = HttpClient::post(
        url,
        body,
        "application/x-www-form-urlencoded");
    if (!response.success) {
        m_lastError = response.error;
        try {
            const auto json = nlohmann::json::parse(response.body);
            m_lastError = json.value(
                "error_description",
                json.value("error", m_lastError));
        } catch (const std::exception&) {
        }
        if (m_lastError.empty()) {
            m_lastError = response.statusCode > 0
                ? "Microsoft returned HTTP " + std::to_string(response.statusCode) + "."
                : "Network request failed while requesting the cloud connection code.";
        }
        return false;
    }

    try {
        const auto json = nlohmann::json::parse(response.body);
        outResponse.device_code = json.at("device_code").get<std::string>();
        outResponse.user_code = json.at("user_code").get<std::string>();
        outResponse.verification_uri = json.value(
            "verification_uri",
            std::string("https://www.microsoft.com/link"));
        outResponse.interval = json.value("interval", 5);
        outResponse.expires_in = json.value("expires_in", 900);
        if (outResponse.device_code.empty() || outResponse.user_code.empty()) {
            m_lastError = "Microsoft returned an incomplete cloud connection code.";
            return false;
        }
        return true;
    } catch (const std::exception&) {
        m_lastError = "Microsoft returned an invalid cloud connection code response.";
        return false;
    }
}

bool AuthManager::pollForTokens(
    const std::string& deviceCode,
    int intervalSec,
    int timeoutSec,
    const std::atomic<bool>* stopRequested) {
    std::string url = "https://login.microsoftonline.com/consumers/oauth2/v2.0/token";
    std::string body = "grant_type=urn:ietf:params:oauth:grant-type:device_code"
                       "&client_id=" + m_clientId +
                       "&device_code=" + deviceCode;

    auto startTime = std::chrono::steady_clock::now();
    int sleepSec = intervalSec > 0 ? intervalSec : 5;

    printf("[Auth] Waiting for user confirmation at microsoft.com/link...\n");

    while (true) {
        auto now = std::chrono::steady_clock::now();
        int elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count();
        if (elapsed > timeoutSec) {
            printf("[Auth] Device code authentication timed out.\n");
            return false;
        }

        for (int remainingMs = sleepSec * 1000; remainingMs > 0; remainingMs -= 100) {
            if (stopRequested && stopRequested->load()) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min(remainingMs, 100)));
        }
        if (stopRequested && stopRequested->load()) return false;

        HttpResponse resp = HttpClient::post(url, body, "application/x-www-form-urlencoded");
        if (resp.success) {
            try {
                auto j = json::parse(resp.body);
                if (j.contains("access_token")) {
                    m_accessToken = j["access_token"].get<std::string>();
                    m_refreshToken = j.value("refresh_token", "");
                    printf("[Auth] Successfully logged into Microsoft account!\n");
                    return true;
                }
            } catch (const std::exception& e) {
                printf("[Auth] Failed to parse token response: %s\n", e.what());
                return false;
            }
        } else {
            try {
                auto j = json::parse(resp.body);
                std::string err = j.value("error", "");
                if (err == "authorization_pending") {
                    // Normal state: user hasn't typed code yet
                    continue;
                } else if (err == "slow_down") {
                    sleepSec += 5;
                    continue;
                } else {
                    printf("[Auth] Polling error: %s\n", err.c_str());
                    return false;
                }
            } catch (const std::exception& e) {
                printf("[Auth] Polling request failed (HTTP %d): %s\n", resp.statusCode, e.what());
                return false;
            }
        }
    }
}

bool AuthManager::pollForConsoleTransferToken(
    const std::string& deviceCode,
    int intervalSec,
    int timeoutSec,
    std::string& transferToken,
    const std::atomic<bool>* stopRequested) {
    transferToken.clear();
    if (deviceCode.empty()) {
        m_lastError = "Microsoft cloud connection code is missing.";
        return false;
    }

    const std::string url = "https://login.live.com/oauth20_token.srf";
    const std::string body =
        "client_id=" + formEncode(m_clientId) +
        "&device_code=" + formEncode(deviceCode) +
        "&grant_type=" + formEncode("urn:ietf:params:oauth:grant-type:device_code");
    const auto startTime = std::chrono::steady_clock::now();
    int sleepSec = intervalSec > 0 ? intervalSec : 5;

    while (true) {
        const auto now = std::chrono::steady_clock::now();
        const int elapsed = static_cast<int>(
            std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count());
        if (elapsed >= timeoutSec) {
            m_lastError = "Cloud connection sign-in expired.";
            return false;
        }

        for (int remainingMs = sleepSec * 1000; remainingMs > 0; remainingMs -= 100) {
            if (stopRequested && stopRequested->load()) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min(remainingMs, 100)));
        }
        if (stopRequested && stopRequested->load()) return false;

        const HttpResponse response = HttpClient::post(
            url,
            body,
            "application/x-www-form-urlencoded");
        if (response.success) {
            try {
                const auto json = nlohmann::json::parse(response.body);
                transferToken = json.value("lpt", json.value("access_token", std::string()));
                if (!transferToken.empty()) return true;
                m_lastError = "Microsoft returned no cloud connection token.";
                return false;
            } catch (const std::exception&) {
                m_lastError = "Microsoft returned an invalid cloud connection token response.";
                return false;
            }
        }

        try {
            const auto json = nlohmann::json::parse(response.body);
            const std::string error = json.value("error", std::string());
            if (error == "authorization_pending") continue;
            if (error == "slow_down") {
                sleepSec += 5;
                continue;
            }
            m_lastError = json.value(
                "error_description",
                error.empty() ? "Cloud connection sign-in was rejected." : error);
            return false;
        } catch (const std::exception&) {
            m_lastError = response.statusCode > 0
                ? "Microsoft returned HTTP " + std::to_string(response.statusCode) +
                      " during cloud connection sign-in."
                : (response.error.empty()
                       ? "Network request failed during cloud connection sign-in."
                       : response.error);
            return false;
        }
    }
}

bool AuthManager::authenticateXboxLive() {
    if (m_accessToken.empty()) {
        printf("[Auth] No Microsoft access token available.\n");
        return false;
    }

    // 1. Authenticate with Xbox Live User service
    printf("[Auth] Requesting Xbox Live user token...\n");
    std::string userAuthUrl = "https://user.auth.xboxlive.com/user/authenticate";
    json userAuthReq = {
        {"RelyingParty", "http://auth.xboxlive.com"},
        {"TokenType", "JWT"},
        {"Properties", {
            {"AuthMethod", "RPS"},
            {"SiteName", "user.auth.xboxlive.com"},
            {"RpsTicket", "d=" + m_accessToken}
        }}
    };

    HttpResponse userResp = HttpClient::post(userAuthUrl, userAuthReq.dump());
    if (!userResp.success) {
        printf("[Auth] User authentication failed. HTTP %d\n", userResp.statusCode);
        return false;
    }

    try {
        auto j = json::parse(userResp.body);
        m_tokens.user_token = j["Token"].get<std::string>();
        m_tokens.user_hash = j["DisplayClaims"]["xui"][0]["uhs"].get<std::string>();
    } catch (const std::exception& e) {
        printf("[Auth] Failed to parse Xbox user token: %s\n", e.what());
        return false;
    }

    // 2. Request XSTS Token for xCloud (Game Streaming)
    printf("[Auth] Requesting Xbox XSTS streaming authorization...\n");
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
        printf("[Auth] XSTS authentication failed. HTTP %d: %s\n", xstsResp.statusCode, xstsResp.body.c_str());
        return false;
    }

    try {
        auto j = json::parse(xstsResp.body);
        m_tokens.xsts_token = j["Token"].get<std::string>();
        if (j["DisplayClaims"]["xui"][0].contains("gtg")) {
            m_tokens.gamertag = j["DisplayClaims"]["xui"][0]["gtg"].get<std::string>();
            printf("[Auth] Authenticated as Gamertag: %s\n", m_tokens.gamertag.c_str());
        }
        return true;
    } catch (const std::exception& e) {
        printf("[Auth] Failed to parse XSTS token: %s\n", e.what());
        return false;
    }
}

} // namespace xcloud
