#include "network_probe.hpp"

#include <cstddef>

#include <orbis/Net.h>
#include <orbis/Sysmodule.h>

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace xcloud {
namespace {

constexpr int SocketNonBlockingOption = 0x1200;

void report(
    std::vector<NetworkProbeCheck>& checks,
    const char* operation,
    int result,
    int errorCode,
    bool passed) {
    printf(
        "[NetworkProbe] %-22s result=%d errno=%d status=%s\n",
        operation,
        result,
        errorCode,
        passed ? "PASS" : "FAIL");
    checks.push_back({operation, result, errorCode, passed});
}

bool setNonBlocking(int descriptor, const char* label, std::vector<NetworkProbeCheck>& checks) {
    errno = 0;
    const int flags = fcntl(descriptor, F_GETFL, 0);
    report(checks, label, flags, flags < 0 ? errno : 0, flags >= 0);
    if (flags >= 0) {
        errno = 0;
        const int result = fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
        report(checks, "fcntl(F_SETFL)", result, result < 0 ? errno : 0, result == 0);
        if (result == 0) return true;
    }

    int enabled = 1;
    errno = 0;
    const int optionResult = setsockopt(
        descriptor,
        SOL_SOCKET,
        SocketNonBlockingOption,
        &enabled,
        sizeof(enabled));
    report(
        checks,
        "setsockopt(SO_NBIO)",
        optionResult,
        optionResult < 0 ? errno : 0,
        optionResult == 0);
    return optionResult == 0;
}

} // namespace

bool runNetworkProbe(std::vector<NetworkProbeCheck>& checks) {
    checks.clear();
    printf("[NetworkProbe] Starting isolated POSIX networking checks.\n");
    errno = 0;
    const int moduleResult =
        static_cast<int>(sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET));
    report(
        checks,
        "load Net module",
        moduleResult,
        moduleResult < 0 ? errno : 0,
        moduleResult >= 0);
    if (moduleResult < 0) return false;

    errno = 0;
    const int netInitResult = sceNetInit();
    report(
        checks,
        "sceNetInit",
        netInitResult,
        netInitResult < 0 ? errno : 0,
        netInitResult >= 0);
    if (netInitResult < 0) return false;

    errno = 0;
    const int poolId = sceNetPoolCreate("xcloudProbe", 64 * 1024, 0);
    report(
        checks,
        "sceNetPoolCreate",
        poolId,
        poolId < 0 ? errno : 0,
        poolId >= 0);
    if (poolId < 0) {
        sceNetTerm();
        return false;
    }

    bool passed = true;
    errno = 0;
    const int socketFd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    report(checks, "socket(UDP)", socketFd, socketFd < 0 ? errno : 0, socketFd >= 0);
    if (socketFd < 0) {
        passed = false;
    } else {
        sockaddr_in address{};
        address.sin_len = sizeof(address);
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        errno = 0;
        const int bindResult =
            bind(socketFd, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        report(checks, "bind(loopback)", bindResult, bindResult < 0 ? errno : 0, bindResult == 0);
        passed = passed && bindResult == 0;

        if (bindResult == 0) {
            socklen_t addressLength = sizeof(address);
            errno = 0;
            const int nameResult =
                getsockname(socketFd, reinterpret_cast<sockaddr*>(&address), &addressLength);
            const bool nameOk = nameResult == 0 && address.sin_port != 0;
            report(checks, "getsockname", nameResult, nameResult < 0 ? errno : 0, nameOk);
            passed = passed && nameOk;
        }

        if (bindResult == 0) {
            const bool nonBlocking = setNonBlocking(socketFd, "fcntl(F_GETFL)", checks);
            passed = passed && nonBlocking;
            const char payload = 'x';
            errno = 0;
            const ssize_t sendResult = sendto(
                socketFd,
                &payload,
                sizeof(payload),
                0,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address));
            const bool sendOk = sendResult == static_cast<ssize_t>(sizeof(payload));
            report(checks, "sendto(loopback)", static_cast<int>(sendResult), sendResult < 0 ? errno : 0, sendOk);
            passed = passed && sendOk;

            pollfd ready{socketFd, POLLIN, 0};
            errno = 0;
            const int pollResult = poll(&ready, 1, 500);
            const bool pollOk = pollResult == 1 && (ready.revents & POLLIN) != 0;
            report(checks, "poll(UDP)", pollResult, pollResult < 0 ? errno : 0, pollOk);
            passed = passed && pollOk;

            if (pollOk) {
                char received = 0;
                errno = 0;
                const ssize_t recvResult = recvfrom(
                    socketFd,
                    &received,
                    sizeof(received),
                    0,
                    nullptr,
                    nullptr);
                const bool recvOk = recvResult == static_cast<ssize_t>(sizeof(received)) &&
                                    received == payload;
                report(
                    checks,
                    "recvfrom(loopback)",
                    static_cast<int>(recvResult),
                    recvResult < 0 ? errno : 0,
                    recvOk);
                passed = passed && recvOk;
            } else {
                report(checks, "recvfrom(loopback)", -1, 0, false);
                passed = false;
            }
        }
        errno = 0;
        const int closeResult = close(socketFd);
        report(checks, "close(UDP)", closeResult, closeResult < 0 ? errno : 0, closeResult == 0);
        passed = passed && closeResult == 0;
    }

    int pipeFds[2] = {-1, -1};
    errno = 0;
    const int pipeResult = pipe(pipeFds);
    report(checks, "pipe", pipeResult, pipeResult < 0 ? errno : 0, pipeResult == 0);
    if (pipeResult < 0) {
        passed = false;
    } else {
        const bool nonBlocking = setNonBlocking(pipeFds[0], "fcntl(pipe F_GETFL)", checks);
        const char payload = 'p';
        errno = 0;
        const ssize_t writeResult = write(pipeFds[1], &payload, sizeof(payload));
        report(checks, "write(pipe)", static_cast<int>(writeResult), writeResult < 0 ? errno : 0, writeResult == 1);
        pollfd ready{pipeFds[0], POLLIN, 0};
        errno = 0;
        const int pollResult = poll(&ready, 1, 500);
        const bool pollOk = pollResult == 1 && (ready.revents & POLLIN) != 0;
        report(checks, "poll(pipe)", pollResult, pollResult < 0 ? errno : 0, pollOk);
        char received = 0;
        errno = 0;
        const ssize_t readResult = read(pipeFds[0], &received, sizeof(received));
        const bool readOk = readResult == 1 && received == payload;
        report(checks, "read(pipe)", static_cast<int>(readResult), readResult < 0 ? errno : 0, readOk);
        passed = passed && nonBlocking && writeResult == 1 && pollOk && readOk;
        close(pipeFds[0]);
        close(pipeFds[1]);
    }

    sceNetPoolDestroy(poolId);
    sceNetTerm();
    printf("[NetworkProbe] Overall result: %s.\n", passed ? "PASS" : "FAIL");
    return passed;
}

} // namespace xcloud
