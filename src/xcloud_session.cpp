#include "xcloud_session.hpp"

#include "http_client.hpp"
#include "json.hpp"

#include <algorithm>
#include <chrono>
#include <set>
#include <thread>
#include <unordered_map>
#include <utility>

namespace xcloud {
namespace {

constexpr const char* GssvClientHeader = "xCloud-PS4";
constexpr const char* OfferingId = "xgpuweb";

std::string urlEncode(const std::string& value) {
    static constexpr char Hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());
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

std::string responseError(const HttpResponse& response) {
    if (response.statusCode > 0) {
        std::string message = "HTTP " + std::to_string(response.statusCode);
        if (!response.body.empty()) {
            try {
                const auto json = nlohmann::json::parse(response.body);
                const std::string detail = json.value(
                    "message",
                    json.value("errorMessage", json.value("error", std::string())));
                if (!detail.empty()) message += ": " + detail;
            } catch (const std::exception&) {
            }
        }
        return message;
    }
    return response.error.empty() ? "Network request failed." : response.error;
}

std::string lookupKnownTitleName(const std::string& productId) {
    static const std::unordered_map<std::string, const char*> KnownTitles = {
        {"BT5P2X999VH2", "Fortnite (Free-to-Play)"},
        {"9PN198B459VB", "007 First Light (Pre-Order)"},
        {"9NBLGGH4NNS1", "Halo Infinite"},
        {"9NP0N1ZJ0P54", "Forza Horizon 5"},
        {"9NC1F6V5BGS9", "Starfield"},
        {"9NBLGGH2JHXJ", "Minecraft"},
        {"9N8D8NHQCWHF", "Sea of Thieves"},
        {"9NX1NPRCH0JS", "Fallout 76"},
        {"9NX0N64T7D20", "Microsoft Flight Simulator"},
        {"9PFK67K4B51V", "Gears 5"},
        {"9N4F4P61D8L8", "Doom Eternal"},
        {"9PJX64273Q67", "Death Stranding"},
        {"9PC1LXZ01JPT", "Hi-Fi RUSH"},
        {"9P9T39D7M41X", "Lies of P"},
        {"9NWLWDZLM96M", "Persona 3 Reload"},
        {"9PD9WT4N3P1K", "Palworld"},
        {"9PF32L7RVTTH", "Dead Space"},
        {"9N1K3F8V31T7", "No Man's Sky"},
        {"9NXZK8MVF72F", "Assassin's Creed Valhalla"},
        {"9PBM727XZDLR", "Grand Theft Auto V"},
        {"9N3K1RZXF7X5", "Cyberpunk 2077"},
        {"9P1S3Z9131C8", "Rainbow Six Siege"},
        {"9MSQW2GZ8F91", "Roblox"},
        {"9N0BNDJBR3VT", "Fallout 4"},
        {"9N6Z7N7F6V90", "The Elder Scrolls V: Skyrim"}
    };
    const auto it = KnownTitles.find(productId);
    return it != KnownTitles.end() ? it->second : "";
}

} // namespace

bool XcloudSession::login(const std::string& xstsToken, std::string& error) {
    m_baseUri.clear();
    m_gameStreamToken.clear();
    m_sessionPath.clear();
    if (xstsToken.empty()) {
        error = "Xbox sign-in did not provide an XSTS token.";
        return false;
    }

    const std::string url =
        "https://" + std::string(OfferingId) + ".gssv-play-prod.xboxlive.com/v2/login/user";
    const nlohmann::json body = {
        {"token", xstsToken},
        {"offeringId", OfferingId}
    };
    const HttpResponse response = HttpClient::post(
        url,
        body.dump(),
        "application/json",
        {{"x-gssv-client", GssvClientHeader}});
    if (!response.success) {
        error = "Cloud service login failed: " + responseError(response);
        return false;
    }

    try {
        const auto json = nlohmann::json::parse(response.body);
        const std::string token = json.value("gsToken", std::string());
        if (token.empty()) {
            error = "Cloud service response did not contain a session token.";
            return false;
        }

        const auto& regions = json.at("offeringSettings").at("regions");
        if (!regions.is_array()) {
            error = "Cloud service returned an invalid region list.";
            return false;
        }

        std::string defaultUri;
        for (const auto& region : regions) {
            if (!region.is_object() || !region.value("isDefault", false)) continue;
            defaultUri = region.value("baseUri", std::string());
            break;
        }
        if (defaultUri.compare(0, 8, "https://") != 0) {
            error = "Cloud service did not return a valid default region.";
            return false;
        }
        while (defaultUri.size() > 8 && defaultUri.back() == '/') defaultUri.pop_back();

        m_gameStreamToken = token;
        m_baseUri = defaultUri;
        printf("[xCloud] Service login succeeded; selected default region.\n");
        return true;
    } catch (const std::exception&) {
        error = "Cloud service returned an invalid login response.";
        return false;
    }
}

bool XcloudSession::listTitles(std::vector<CloudTitle>& titles, std::string& error) const {
    titles.clear();
    if (m_baseUri.empty() || m_gameStreamToken.empty()) {
        error = "Cloud service login is required before loading the catalog.";
        return false;
    }

    constexpr size_t MaxCatalogPages = 50;
    constexpr size_t MaxCatalogTitles = 5000;
    std::vector<CloudTitle> loadedTitles;
    std::set<std::string> seenContinuationTokens;
    std::string continuationToken;

    for (size_t page = 0; page < MaxCatalogPages; ++page) {
        std::string url = m_baseUri + "/v2/titles?mr=200";
        if (!continuationToken.empty()) {
            url += "&ct=" + urlEncode(continuationToken);
        }
        const HttpResponse response = HttpClient::get(
            url,
            {
                {"Authorization", "Bearer " + m_gameStreamToken},
                {"x-gssv-client", GssvClientHeader}
            });
        if (!response.success) {
            error = "Cloud catalog request failed: " + responseError(response);
            return false;
        }

        try {
            const auto json = nlohmann::json::parse(response.body);
            const auto& results = json.at("results");
            if (!results.is_array()) {
                error = "Cloud service returned an invalid title list.";
                return false;
            }
            for (const auto& item : results) {
                if (!item.is_object()) continue;
                CloudTitle title;
                title.titleId = item.value("titleId", std::string());
                if (title.titleId.empty()) continue;
                if (item.contains("details") && item["details"].is_object()) {
                    const auto& details = item["details"];
                    title.productId = details.value("productId", std::string());
                    title.titleName = details.value("productTitle",
                        details.value("title",
                        details.value("name", std::string())));
                    title.entitled = details.value("hasEntitlement", false);
                    title.freeInStore = details.value("isFreeInStore", false);
                }
                if (title.titleName.empty()) {
                    title.titleName = lookupKnownTitleName(title.productId);
                }
                if (title.titleName.empty()) {
                    title.titleName = item.value("productTitle",
                        item.value("name",
                        item.value("title", std::string())));
                }
                if (title.titleName.empty()) {
                    title.titleName = "Xbox Title (" + (title.productId.empty() ? title.titleId : title.productId) + ")";
                }
                loadedTitles.push_back(std::move(title));
                if (loadedTitles.size() > MaxCatalogTitles) {
                    error = "Cloud catalog exceeded the title safety limit.";
                    return false;
                }
            }

            std::string nextToken;
            if (json.contains("continuationToken") && !json["continuationToken"].is_null()) {
                if (!json["continuationToken"].is_string()) {
                    error = "Cloud service returned an invalid catalog continuation token.";
                    return false;
                }
                nextToken = json["continuationToken"].get<std::string>();
            }
            if (nextToken.empty()) {
                // Prioritize playable titles (entitled or free) to the top!
                std::stable_sort(loadedTitles.begin(), loadedTitles.end(), [](const CloudTitle& a, const CloudTitle& b) {
                    const bool aPlayable = a.entitled || a.freeInStore || a.productId == "BT5P2X999VH2";
                    const bool bPlayable = b.entitled || b.freeInStore || b.productId == "BT5P2X999VH2";
                    if (aPlayable != bPlayable) return aPlayable > bPlayable;
                    if ((a.productId == "BT5P2X999VH2") != (b.productId == "BT5P2X999VH2")) {
                        return a.productId == "BT5P2X999VH2";
                    }
                    return false;
                });

                titles.swap(loadedTitles);
                printf("[xCloud] Loaded and sorted %zu catalog entries across %zu page(s).\n",
                       titles.size(),
                       page + 1);
                return true;
            }
            if (!seenContinuationTokens.insert(nextToken).second) {
                error = "Cloud catalog returned a repeated continuation token.";
                return false;
            }
            if (page + 1 == MaxCatalogPages) {
                error = "Cloud catalog exceeded the pagination safety limit.";
                return false;
            }
            continuationToken = nextToken;
        } catch (const std::exception&) {
            error = "Cloud service returned an invalid title catalog.";
            return false;
        }
    }
    error = "Cloud catalog pagination did not complete.";
    return false;
}

bool XcloudSession::startSession(const std::string& titleId, std::string& error) {
    if (m_baseUri.empty() || m_gameStreamToken.empty()) {
        error = "Cloud service login is required before starting a game.";
        return false;
    }
    if (titleId.empty()) {
        error = "The selected cloud title has no session ID.";
        return false;
    }
    if (!m_sessionPath.empty()) {
        error = "A cloud session is already active.";
        return false;
    }

    char clientSessionId[64];
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::snprintf(clientSessionId, sizeof(clientSessionId), "f47ac10b-58cc-4372-a567-%012llx",
                  static_cast<unsigned long long>(now & 0xFFFFFFFFFFFFULL));

    const nlohmann::json body = {
        {"clientSessionId", std::string(clientSessionId)},
        {"titleId", titleId},
        {"systemUpdateGroup", ""},
        {"settings", {
            {"nanoVersion", "V3;WebrtcTransport.dll"},
            {"enableTextToSpeech", false},
            {"highContrast", 0},
            {"locale", "en-US"},
            {"useIceConnection", false},
            {"timezoneOffsetMinutes", 0},
            {"sdkType", "web"},
            {"osName", "windows"}
        }},
        {"serverId", ""},
        {"fallbackRegionNames", nlohmann::json::array()}
    };
    const HttpResponse response = HttpClient::post(
        m_baseUri + "/v5/sessions/cloud/play",
        body.dump(),
        "application/json",
        {
            {"Authorization", "Bearer " + m_gameStreamToken},
            {"x-gssv-client", GssvClientHeader}
        });
    if (!response.success) {
        error = "Could not start the cloud session: " + responseError(response);
        return false;
    }

    try {
        const auto json = nlohmann::json::parse(response.body);
        std::string sessionPath = json.value("sessionPath", std::string());
        if (sessionPath.empty() || sessionPath.find("://") != std::string::npos ||
            sessionPath.compare(0, 2, "//") == 0) {
            error = "Cloud service returned an invalid session path.";
            return false;
        }
        if (sessionPath.front() != '/') sessionPath.insert(sessionPath.begin(), '/');
        m_sessionPath = sessionPath;
        printf("[xCloud] Cloud session request accepted.\n");
        return true;
    } catch (const std::exception&) {
        error = "Cloud service returned an invalid session response.";
        return false;
    }
}

bool XcloudSession::getSessionState(std::string& state, std::string& error) const {
    state.clear();
    if (m_sessionPath.empty()) {
        error = "No cloud session is active.";
        return false;
    }

    const HttpResponse response = HttpClient::get(
        m_baseUri + m_sessionPath + "/state",
        {
            {"Authorization", "Bearer " + m_gameStreamToken},
            {"x-gssv-client", GssvClientHeader}
        });
    if (!response.success) {
        error = "Could not check cloud session status: " + responseError(response);
        return false;
    }

    try {
        const auto json = nlohmann::json::parse(response.body);
        state = json.value("state", std::string());
        if (state.empty()) {
            error = "Cloud service returned no session state.";
            return false;
        }
        if (state == "Failed") {
            const auto& detail = json.value("errorDetails", nlohmann::json::object());
            const std::string code = detail.value("code", std::string());
            const std::string message = detail.value("message", std::string());
            error = "Cloud session failed";
            if (!code.empty()) error += " (" + code + ")";
            if (!message.empty()) error += ": " + message;
            return false;
        }
        return true;
    } catch (const std::exception&) {
        error = "Cloud service returned an invalid session state.";
        return false;
    }
}

bool XcloudSession::connectSession(const std::string& transferToken, std::string& error) const {
    if (m_sessionPath.empty()) {
        error = "No cloud session is active.";
        return false;
    }
    if (transferToken.empty()) {
        error = "Microsoft did not provide a cloud connection token.";
        return false;
    }

    const nlohmann::json body = {
        {"userToken", transferToken}
    };
    const HttpResponse response = HttpClient::post(
        m_baseUri + m_sessionPath + "/connect",
        body.dump(),
        "application/json",
        {
            {"Authorization", "Bearer " + m_gameStreamToken},
            {"x-gssv-client", GssvClientHeader}
        });
    if (!response.success) {
        error = "Could not connect the cloud session: " + responseError(response);
        return false;
    }
    printf("[xCloud] Session client handoff accepted.\n");
    return true;
}

void XcloudSession::stopSession() {
    if (m_sessionPath.empty()) return;
    const HttpResponse response = HttpClient::get(
        m_baseUri + m_sessionPath,
        {
            {"Authorization", "Bearer " + m_gameStreamToken},
            {"x-gssv-client", GssvClientHeader}
        },
        "DELETE");
    if (!response.success) {
        printf("[xCloud] Session cleanup request failed: %s\n", responseError(response).c_str());
        return;
    }
    m_sessionPath.clear();
    printf("[xCloud] Cloud session stopped.\n");
}

static bool parseSdpAnswerFromBody(const std::string& body, std::string& answerSdp) {
    answerSdp.clear();
    if (body.empty()) return false;
    if (body.compare(0, 3, "v=0") == 0) {
        answerSdp = body;
        return true;
    }
    try {
        const auto json = nlohmann::json::parse(body);
        if (json.contains("exchangeResponse")) {
            if (json["exchangeResponse"].is_object()) {
                answerSdp = json["exchangeResponse"].value("sdp", std::string());
            } else if (json["exchangeResponse"].is_string()) {
                const std::string raw = json["exchangeResponse"].get<std::string>();
                try {
                    const auto inner = nlohmann::json::parse(raw);
                    answerSdp = inner.value("sdp", std::string());
                } catch (...) {
                    if (raw.find("v=0") != std::string::npos) {
                        answerSdp = raw;
                    }
                }
            }
        }
        if (answerSdp.empty() && json.contains("sdp") && json["sdp"].is_string()) {
            answerSdp = json["sdp"].get<std::string>();
        }
        if (answerSdp.empty() && json.is_array() && !json.empty()) {
            const auto& first = json[0];
            if (first.is_object() && first.contains("sdp") && first["sdp"].is_string()) {
                answerSdp = first["sdp"].get<std::string>();
            }
        }
        return !answerSdp.empty();
    } catch (...) {
        const size_t v0 = body.find("v=0");
        if (v0 != std::string::npos) {
            answerSdp = body.substr(v0);
            return true;
        }
        return false;
    }
}

bool XcloudSession::sendSdpOffer(const std::string& offerSdp, std::string& outAnswerSdp, std::string& error) const {
    outAnswerSdp.clear();
    if (m_sessionPath.empty()) {
        error = "No active cloud session for SDP offer.";
        return false;
    }
    const auto version = [](int minimum, int maximum) {
        return nlohmann::json{{"minVersion", minimum}, {"maxVersion", maximum}};
    };
    const nlohmann::json body = {
        {"messageType", "offer"},
        {"sdp", offerSdp},
        {"requestId", "1"},
        {"configuration", {
            {"chatConfiguration", {
                {"bytesPerSample", 2},
                {"expectedClipDurationMs", 20},
                {"format", {
                    {"codec", "opus"},
                    {"container", "webm"}
                }},
                {"numChannels", 1},
                {"sampleFrequencyHz", 24000}
            }},
            {"chat", version(1, 1)},
            {"control", version(1, 3)},
            {"input", version(1, 9)},
            {"message", version(1, 1)}
        }}
    };
    const HttpResponse response = HttpClient::post(
        m_baseUri + m_sessionPath + "/sdp",
        body.dump(),
        "application/json",
        {
            {"Authorization", "Bearer " + m_gameStreamToken},
            {"x-gssv-client", GssvClientHeader}
        });
    if (!response.success && response.statusCode != 200 && response.statusCode != 202 && response.statusCode != 204) {
        error = "Failed to send SDP offer: " + responseError(response);
        return false;
    }
    printf("[xCloud] WebRTC SDP offer sent successfully (HTTP %d, body %zu bytes).\n",
           response.statusCode, response.body.size());

    // Check if SDP answer was returned directly in POST response
    if (parseSdpAnswerFromBody(response.body, outAnswerSdp)) {
        printf("[xCloud] Found SDP answer inline in POST /sdp response!\n");
    }
    return true;
}

bool XcloudSession::querySdpAnswer(std::string& answerSdp, std::string& error) const {
    answerSdp.clear();
    if (m_sessionPath.empty()) {
        error = "No active cloud session for SDP query.";
        return false;
    }
    const HttpResponse response = HttpClient::get(
        m_baseUri + m_sessionPath + "/sdp",
        {
            {"Authorization", "Bearer " + m_gameStreamToken},
            {"x-gssv-client", GssvClientHeader}
        });
    if (!response.success) {
        error = "Failed to query SDP answer: " + responseError(response);
        return false;
    }

    return parseSdpAnswerFromBody(response.body, answerSdp);
}

bool XcloudSession::exchangeSdp(
    const std::string& offerSdp,
    std::string& answerSdp,
    std::string& error,
    const std::function<void(const std::string&)>& statusCb) const {
    answerSdp.clear();

    if (statusCb) statusCb("Submitting WebRTC SDP offer to blade...");

    const auto offerDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    bool offerSent = false;
    while (std::chrono::steady_clock::now() < offerDeadline) {
        if (sendSdpOffer(offerSdp, answerSdp, error)) {
            offerSent = true;
            break;
        }
        if (error.find("provisioned") != std::string::npos || error.find("400") != std::string::npos) {
            if (statusCb) statusCb("Waiting for blade to finish provisioning...");
            printf("[xCloud] Waiting for session to become provisioned before SDP offer...\n");
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        return false;
    }
    if (!offerSent) {
        if (error.empty()) error = "Timed out waiting for xCloud to provision the streaming session.";
        return false;
    }

    if (!answerSdp.empty()) {
        if (statusCb) statusCb("SDP answer received inline! Initializing pipeline...");
        printf("[xCloud] SDP answer received inline (%zu bytes).\n", answerSdp.size());
        return true;
    }

    if (statusCb) statusCb("SDP offer sent. Awaiting blade SDP answer...");

    // 3. Fast poll for the answer SDP
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    int pollCount = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        if (querySdpAnswer(answerSdp, error) && !answerSdp.empty()) {
            if (statusCb) statusCb("SDP answer received! Negotiating media stream...");
            printf("[xCloud] SDP answer received via GET (%zu bytes).\n", answerSdp.size());
            return true;
        }
        ++pollCount;
        if (statusCb) {
            statusCb("Awaiting blade SDP answer (poll " + std::to_string(pollCount) + ")...");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    error = "Timed out waiting for SDP answer from xCloud.";
    return false;
}

bool XcloudSession::exchangeIceCandidates(
    const std::vector<WebRtcSignaledCandidate>& localCandidates,
    std::vector<WebRtcSignaledCandidate>& remoteCandidates,
    std::string& error,
    const std::function<void(const std::string&)>& statusCb) const {
    remoteCandidates.clear();
    if (m_sessionPath.empty()) {
        error = "No active cloud session for ICE exchange.";
        return false;
    }
    if (statusCb) statusCb("Sending gathered ICE candidates to the cloud session...");

    nlohmann::json candidateList = nlohmann::json::array();
    for (const auto& candidate : localCandidates) {
        candidateList.push_back({
            {"candidate", candidate.candidate},
            {"sdpMid", candidate.sdpMid},
            {"sdpMLineIndex", candidate.sdpMLineIndex}
        });
    }
    const nlohmann::json body = {{"messageType", "iceCandidate"}, {"candidate", candidateList}};
    const HttpResponse upload = HttpClient::post(
        m_baseUri + m_sessionPath + "/ice", body.dump(), "application/json",
        {{"Authorization", "Bearer " + m_gameStreamToken}, {"x-gssv-client", GssvClientHeader}});
    if (!upload.success) {
        error = "Failed to send ICE candidates: " + responseError(upload);
        return false;
    }

    if (statusCb) statusCb("Waiting for the cloud session's ICE candidates...");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        const HttpResponse response = HttpClient::get(
            m_baseUri + m_sessionPath + "/ice",
            {{"Authorization", "Bearer " + m_gameStreamToken}, {"x-gssv-client", GssvClientHeader}});
        if (response.statusCode == 204) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            continue;
        }
        if (!response.success) {
            error = "Failed to receive ICE candidates: " + responseError(response);
            return false;
        }
        try {
            nlohmann::json payload = nlohmann::json::parse(response.body);
            if (payload.is_object() && payload.contains("exchangeResponse")) {
                auto exchange = payload["exchangeResponse"];
                if (exchange.is_string()) exchange = nlohmann::json::parse(exchange.get<std::string>());
                payload = std::move(exchange);
            }
            if (payload.is_object() && payload.contains("candidates")) payload = payload["candidates"];
            if (payload.is_object() && payload.contains("candidate")) payload = payload["candidate"];
            auto appendCandidate = [&remoteCandidates](const nlohmann::json& item) {
                if (!item.is_object()) return;
                WebRtcSignaledCandidate candidate;
                candidate.candidate = item.value("candidate", std::string());
                candidate.sdpMid = item.value("sdpMid", std::string());
                candidate.sdpMLineIndex = item.value("sdpMLineIndex", 0);
                if (!candidate.candidate.empty()) remoteCandidates.push_back(std::move(candidate));
            };
            if (payload.is_array()) {
                for (const auto& item : payload) appendCandidate(item);
            } else if (payload.is_object()) {
                appendCandidate(payload);
                if (remoteCandidates.empty()) {
                    for (auto it = payload.begin(); it != payload.end(); ++it) appendCandidate(it.value());
                }
            }
        } catch (const std::exception&) {
            error = "Cloud service returned invalid ICE candidate data.";
            return false;
        }
        if (!remoteCandidates.empty()) {
            printf("[xCloud] Received %zu remote ICE candidate(s).\n", remoteCandidates.size());
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    error = "Timed out waiting for ICE candidates from the cloud session.";
    return false;
}

} // namespace xcloud
