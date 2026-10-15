#pragma once

#include <string>
#include <vector>

namespace xcloud {

struct CloudTitle {
    std::string titleId;
    std::string productId;
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

private:
    std::string m_baseUri;
    std::string m_gameStreamToken;
    std::string m_sessionPath;
};

} // namespace xcloud
