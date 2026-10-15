#include "xcloud_session.hpp"

#include "http_client.hpp"
#include "json.hpp"

#include <set>
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
                    title.entitled = details.value("hasEntitlement", false);
                    title.freeInStore = details.value("isFreeInStore", false);
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
                titles.swap(loadedTitles);
                printf("[xCloud] Loaded %zu catalog entries across %zu page(s).\n",
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

    const nlohmann::json body = {
        {"clientSessionId", ""},
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

} // namespace xcloud
