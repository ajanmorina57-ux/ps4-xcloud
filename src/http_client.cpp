#include "http_client.hpp"
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(__ORBIS__) || defined(__PS4__)
#include <orbis/Http.h>
#include <orbis/Ssl.h>
#include <orbis/Net.h>
#include <orbis/Sysmodule.h>
#endif

namespace xcloud {

int HttpClient::s_libnetMemId = 0;
int HttpClient::s_libhttpCtxId = 0;
int HttpClient::s_libsslCtxId = 0;
bool HttpClient::s_initialized = false;

#if defined(__ORBIS__) || defined(__PS4__)
static int verifySslCallback(int libsslId, unsigned int verifyErr, void * const sslCert[], int certNum, void *userArg) {
    (void)libsslId;
    (void)sslCert;
    (void)certNum;
    (void)userArg;
    return verifyErr == 0;
}

static inline int32_t setRecvTimeOut(int32_t reqId, uint32_t usec) {
    using Func = int32_t (*)(int32_t, uint32_t);
    return reinterpret_cast<Func>(sceHttpSetRecvTimeOut)(reqId, usec);
}
#endif

bool HttpClient::init() {
    if (s_initialized) return true;

#if defined(__ORBIS__) || defined(__PS4__)
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL);

    int ret = sceNetInit();
    if (ret < 0) {
        printf("[HttpClient] sceNetInit failed: 0x%08X\n", ret);
        return false;
    }
    ret = sceNetPoolCreate("netPool", 1024 * 1024, 0);
    if (ret < 0) {
        printf("[HttpClient] sceNetPoolCreate failed: 0x%08X\n", ret);
        return false;
    }
    s_libnetMemId = ret;

    ret = sceSslInit(512 * 1024);
    if (ret < 0) {
        printf("[HttpClient] sceSslInit failed: 0x%08X\n", ret);
        return false;
    }
    s_libsslCtxId = ret;

    ret = sceHttpInit(s_libnetMemId, s_libsslCtxId, 512 * 1024);
    if (ret < 0) {
        printf("[HttpClient] sceHttpInit failed: 0x%08X\n", ret);
        return false;
    }
    s_libhttpCtxId = ret;
#endif

    s_initialized = true;
    return true;
}

void HttpClient::shutdown() {
#if defined(__ORBIS__) || defined(__PS4__)
    if (s_libhttpCtxId > 0) sceHttpTerm(s_libhttpCtxId);
    if (s_libsslCtxId > 0) sceSslTerm();
    if (s_libnetMemId > 0) sceNetPoolDestroy(s_libnetMemId);
    sceNetTerm();
#endif
    s_initialized = false;
}

HttpResponse HttpClient::get(
    const std::string& url,
    const std::map<std::string, std::string>& headers,
    const std::string& method) {
    HttpResponse resp;
    if (method != "GET" && method != "DELETE") {
        resp.error = "Unsupported HTTP method.";
        return resp;
    }
#if defined(__ORBIS__) || defined(__PS4__)
    if (!s_initialized && !init()) {
        resp.error = "HTTP client initialization failed.";
        return resp;
    }

    int tpl = sceHttpCreateTemplate(s_libhttpCtxId, "Mozilla/5.0 (PLAYSTATION 4; xCloud-PS4)", ORBIS_HTTP_VERSION_1_1, 1);
    if (tpl < 0) {
        resp.error = "Could not create HTTP template.";
        return resp;
    }

    sceHttpSetResolveTimeOut(tpl, 10 * 1000 * 1000);
    sceHttpsSetSslCallback(tpl, verifySslCallback, nullptr);

    int conn = sceHttpCreateConnectionWithURL(tpl, url.c_str(), 0);
    if (conn < 0) {
        resp.error = "Could not resolve or connect to the server.";
        sceHttpDeleteTemplate(tpl);
        return resp;
    }
    sceHttpSetConnectTimeOut(conn, 10 * 1000 * 1000);

    const int orbisMethod = method == "DELETE" ? ORBIS_METHOD_DELETE : ORBIS_METHOD_GET;
    int req = sceHttpCreateRequestWithURL(conn, orbisMethod, url.c_str(), 0);
    if (req < 0) {
        resp.error = "Could not create HTTP request.";
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tpl);
        return resp;
    }

    sceHttpSetSendTimeOut(req, 10 * 1000 * 1000);
    setRecvTimeOut(req, 10 * 1000 * 1000);
    sceHttpAddRequestHeader(req, "Connection", "close", 0);

    for (const auto& [key, val] : headers) {
        sceHttpAddRequestHeader(req, key.c_str(), val.c_str(), 0);
    }

    int sendRet = sceHttpSendRequest(req, nullptr, 0);
    if (sendRet == 0) {
        int status = 0;
        sceHttpGetStatusCode(req, &status);
        resp.statusCode = status;
        resp.success = (status >= 200 && status < 300);

        int contentLengthType = 0;
        uint64_t contentLength = 0;
        const int clRet = sceHttpGetResponseContentLength(req, &contentLengthType, &contentLength);
        if (clRet == 0 && contentLengthType == ORBIS_HTTP_CONTENTLEN_EXIST) {
            if (contentLength > 0) {
                resp.body.reserve(static_cast<size_t>(contentLength));
                char buf[2048];
                while (resp.body.size() < contentLength) {
                    const size_t toRead = std::min(sizeof(buf) - 1, static_cast<size_t>(contentLength - resp.body.size()));
                    const int readBytes = sceHttpReadData(req, buf, toRead);
                    if (readBytes <= 0) break;
                    buf[readBytes] = '\0';
                    resp.body.append(buf, readBytes);
                }
            }
        } else {
            char buf[2048];
            int readBytes = 0;
            while ((readBytes = sceHttpReadData(req, buf, sizeof(buf) - 1)) > 0) {
                buf[readBytes] = '\0';
                resp.body.append(buf, readBytes);
            }
        }
    } else {
        int lastErrno = 0;
        sceHttpGetLastErrno(req, &lastErrno);
        resp.error = "HTTP request failed (0x" + std::to_string(static_cast<uint32_t>(sendRet)) +
                     ", network " + std::to_string(lastErrno) + ").";
    }

    sceHttpDeleteRequest(req);
    sceHttpDeleteConnection(conn);
    sceHttpDeleteTemplate(tpl);
#else
    resp.success = true;
    resp.statusCode = 200;
#endif
    return resp;
}

HttpResponse HttpClient::post(const std::string& url, const std::string& body, const std::string& contentType, const std::map<std::string, std::string>& headers) {
    HttpResponse resp;
#if defined(__ORBIS__) || defined(__PS4__)
    if (!s_initialized && !init()) {
        resp.error = "HTTP client initialization failed.";
        return resp;
    }

    int tpl = sceHttpCreateTemplate(s_libhttpCtxId, "Mozilla/5.0 (PLAYSTATION 4; xCloud-PS4)", ORBIS_HTTP_VERSION_1_1, 1);
    if (tpl < 0) {
        resp.error = "Could not create HTTP template.";
        return resp;
    }

    sceHttpSetResolveTimeOut(tpl, 10 * 1000 * 1000);
    sceHttpsSetSslCallback(tpl, verifySslCallback, nullptr);

    int conn = sceHttpCreateConnectionWithURL(tpl, url.c_str(), 0);
    if (conn < 0) {
        resp.error = "Could not resolve or connect to the server.";
        sceHttpDeleteTemplate(tpl);
        return resp;
    }
    sceHttpSetConnectTimeOut(conn, 10 * 1000 * 1000);

    int req = sceHttpCreateRequestWithURL(conn, ORBIS_METHOD_POST, url.c_str(), body.length());
    if (req < 0) {
        resp.error = "Could not create HTTP request.";
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tpl);
        return resp;
    }
    sceHttpSetSendTimeOut(req, 10 * 1000 * 1000);
    setRecvTimeOut(req, 10 * 1000 * 1000);
    sceHttpAddRequestHeader(req, "Connection", "close", 0);

    sceHttpAddRequestHeader(req, "Content-Type", contentType.c_str(), 0);
    for (const auto& [key, val] : headers) {
        sceHttpAddRequestHeader(req, key.c_str(), val.c_str(), 0);
    }

    int sendRet = sceHttpSendRequest(req, body.data(), body.length());
    if (sendRet == 0) {
        int status = 0;
        sceHttpGetStatusCode(req, &status);
        resp.statusCode = status;
        resp.success = (status >= 200 && status < 300);

        int contentLengthType = 0;
        uint64_t contentLength = 0;
        const int clRet = sceHttpGetResponseContentLength(req, &contentLengthType, &contentLength);
        if (clRet == 0 && contentLengthType == ORBIS_HTTP_CONTENTLEN_EXIST) {
            if (contentLength > 0) {
                resp.body.reserve(static_cast<size_t>(contentLength));
                char buf[2048];
                while (resp.body.size() < contentLength) {
                    const size_t toRead = std::min(sizeof(buf) - 1, static_cast<size_t>(contentLength - resp.body.size()));
                    const int readBytes = sceHttpReadData(req, buf, toRead);
                    if (readBytes <= 0) break;
                    buf[readBytes] = '\0';
                    resp.body.append(buf, readBytes);
                }
            }
        } else {
            char buf[2048];
            int readBytes = 0;
            while ((readBytes = sceHttpReadData(req, buf, sizeof(buf) - 1)) > 0) {
                buf[readBytes] = '\0';
                resp.body.append(buf, readBytes);
            }
        }
    } else {
        int lastErrno = 0;
        sceHttpGetLastErrno(req, &lastErrno);
        char errorBuffer[128];
        snprintf(errorBuffer,
                 sizeof(errorBuffer),
                 "HTTP request failed: 0x%08X (network error %d).",
                 static_cast<uint32_t>(sendRet),
                 lastErrno);
        resp.error = errorBuffer;
    }

    sceHttpDeleteRequest(req);
    sceHttpDeleteConnection(conn);
    sceHttpDeleteTemplate(tpl);
#else
    resp.success = true;
    resp.statusCode = 200;
#endif
    return resp;
}

} // namespace xcloud
