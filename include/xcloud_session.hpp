#pragma once

#include <string>
#include <vector>
#include <functional>
#include "webrtc_sdp.hpp"

namespace xcloud {

struct CloudTitle {
    std::string titleId;
    std::string productId;
    std::string titleName;
    bool entitled = false;
    bool freeInStore = false;
};

class XcloudSession {
public:
    bool login(const std::string& xstsToken, std::string& error);
    bool listTitles(std::vector<CloudTitle>& titles, std::string& error) const;
    bool startSession(const std::string& titleId, std::string& error);
    bool getSessionState(std::string& state, std::string& error) const;
    bool connectSession(const std::string& transferToken, std::string& error) const;
    void stopSession();

    // WebRTC SDP and ICE exchange
    bool sendSdpOffer(const std::string& offerSdp, std::string& outAnswerSdp, std::string& error) const;
    bool querySdpAnswer(std::string& answerSdp, std::string& error) const;
    bool exchangeSdp(
        const std::string& offerSdp,
        std::string& answerSdp,
        std::string& error,
        const std::function<void(const std::string&)>& statusCb = nullptr) const;
    bool exchangeIceCandidates(
        const std::vector<WebRtcSignaledCandidate>& localCandidates,
        std::vector<WebRtcSignaledCandidate>& remoteCandidates,
        std::string& error,
        const std::function<void(const std::string&)>& statusCb = nullptr) const;
    const std::string& getSessionPath() const { return m_sessionPath; }

private:
    std::string m_baseUri;
    std::string m_gameStreamToken;
    std::string m_sessionPath;
};

} // namespace xcloud
