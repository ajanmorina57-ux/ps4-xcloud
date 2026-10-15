#pragma once
#include <string>
#include <map>

namespace xcloud {

struct HttpResponse {
    int statusCode = 0;
    std::string body;
    std::string error;
    bool success = false;
};

class HttpClient {
public:
    static bool init();
    static void shutdown();

    static HttpResponse get(
        const std::string& url,
        const std::map<std::string, std::string>& headers = {},
        const std::string& method = "GET");
    static HttpResponse post(const std::string& url, const std::string& body, const std::string& contentType = "application/json", const std::map<std::string, std::string>& headers = {});

private:
    static int s_libnetMemId;
    static int s_libhttpCtxId;
    static int s_libsslCtxId;
    static bool s_initialized;
};

} // namespace xcloud
