#include "webrtc_sdp.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <utility>

namespace xcloud {
namespace {

constexpr size_t MaxSdpSize = 256 * 1024;
constexpr size_t MaxMediaSections = 16;
constexpr size_t MaxCandidatesPerMedia = 256;

bool parseUnsigned(const std::string& value, unsigned int& result) {
    if (value.empty()) return false;
    unsigned int parsed = 0;
    for (const unsigned char character : value) {
        if (!std::isdigit(character)) return false;
        const unsigned int digit = character - '0';
        if (parsed > (std::numeric_limits<unsigned int>::max() - digit) / 10) return false;
        parsed = parsed * 10 + digit;
    }
    result = parsed;
    return true;
}

bool parsePayloadType(const std::string& value, int& result) {
    unsigned int parsed = 0;
    if (!parseUnsigned(value, parsed) || parsed > 127) return false;
    result = static_cast<int>(parsed);
    return true;
}

std::string trim(const std::string& value) {
    const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
    const auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }).base();
    return begin < end ? std::string(begin, end) : std::string();
}

bool parseMediaLine(const std::string& line, SdpMediaSection& media, std::string& error) {
    std::istringstream fields(line.substr(2));
    std::string portField;
    if (!(fields >> media.media >> portField >> media.protocol)) {
        error = "SDP contains an incomplete media line.";
        return false;
    }

    const size_t slash = portField.find('/');
    const std::string port = portField.substr(0, slash);
    if (!parseUnsigned(port, media.port)) {
        error = "SDP contains an invalid media port.";
        return false;
    }

    std::string payload;
    while (fields >> payload) {
        int payloadType = -1;
        if (!parsePayloadType(payload, payloadType)) {
            error = "SDP contains an invalid RTP payload type.";
            return false;
        }
        media.payloadTypes.push_back(payloadType);
    }
    if (media.payloadTypes.empty()) {
        error = "SDP media section has no payload types.";
        return false;
    }
    return true;
}

bool parseRtpMap(const std::string& value, SdpMediaSection& media, std::string& error) {
    const size_t separator = value.find(' ');
    if (separator == std::string::npos) {
        error = "SDP contains an incomplete RTP codec mapping.";
        return false;
    }

    int payloadType = -1;
    if (!parsePayloadType(value.substr(0, separator), payloadType) ||
        std::find(media.payloadTypes.begin(), media.payloadTypes.end(), payloadType) ==
            media.payloadTypes.end()) {
        error = "SDP codec mapping references an invalid media payload type.";
        return false;
    }

    const std::string encoding = trim(value.substr(separator + 1));
    const size_t firstSlash = encoding.find('/');
    if (firstSlash == std::string::npos || firstSlash == 0) {
        error = "SDP contains an invalid RTP codec mapping.";
        return false;
    }
    const size_t secondSlash = encoding.find('/', firstSlash + 1);
    SdpCodec codec;
    codec.payloadType = payloadType;
    codec.name = encoding.substr(0, firstSlash);
    const std::string clockRate = encoding.substr(
        firstSlash + 1,
        secondSlash == std::string::npos ? std::string::npos : secondSlash - firstSlash - 1);
    if (!parseUnsigned(clockRate, codec.clockRate) || codec.clockRate == 0) {
        error = "SDP contains an invalid codec clock rate.";
        return false;
    }
    if (secondSlash != std::string::npos &&
        (!parseUnsigned(encoding.substr(secondSlash + 1), codec.channels) || codec.channels == 0)) {
        error = "SDP contains an invalid codec channel count.";
        return false;
    }

    const auto existing = std::find_if(media.codecs.begin(), media.codecs.end(), [&](const SdpCodec& item) {
        return item.payloadType == payloadType;
    });
    if (existing != media.codecs.end()) {
        *existing = std::move(codec);
    } else {
        media.codecs.push_back(std::move(codec));
    }
    return true;
}

bool parseFingerprint(const std::string& value, std::string& algorithm, std::string& fingerprint) {
    const size_t separator = value.find(' ');
    if (separator == std::string::npos || separator == 0 || separator + 1 >= value.size()) return false;
    const std::string candidateAlgorithm = value.substr(0, separator);
    const std::string candidateFingerprint = trim(value.substr(separator + 1));
    if (candidateFingerprint.empty()) return false;
    for (const unsigned char character : candidateFingerprint) {
        if (!std::isxdigit(character) && character != ':') return false;
    }
    algorithm = candidateAlgorithm;
    fingerprint = candidateFingerprint;
    return true;
}

} // namespace

bool parseSessionDescription(
    const std::string& sdp,
    SessionDescription& description,
    std::string& error) {
    description = {};
    error.clear();
    if (sdp.empty() || sdp.size() > MaxSdpSize) {
        error = "SDP is empty or exceeds the size limit.";
        return false;
    }

    std::istringstream lines(sdp);
    std::string line;
    SdpMediaSection* currentMedia = nullptr;
    size_t lineNumber = 0;
    while (std::getline(lines, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line.size() < 2 || line[1] != '=') {
            error = "SDP contains a malformed line " + std::to_string(lineNumber) + ".";
            description = {};
            return false;
        }

        if (line.compare(0, 2, "m=") == 0) {
            if (description.media.size() >= MaxMediaSections) {
                error = "SDP contains too many media sections.";
                description = {};
                return false;
            }
            description.media.emplace_back();
            currentMedia = &description.media.back();
            if (!parseMediaLine(line, *currentMedia, error)) {
                description = {};
                return false;
            }
            continue;
        }
        if (line.compare(0, 2, "a=") != 0) continue;

        const std::string attribute = line.substr(2);
        auto assignNonEmpty = [&](const std::string& prefix, std::string& target) {
            if (attribute.compare(0, prefix.size(), prefix) != 0) return false;
            const std::string value = trim(attribute.substr(prefix.size()));
            if (value.empty()) {
                error = "SDP contains an empty " + prefix.substr(0, prefix.size() - 1) + " attribute.";
                return true;
            }
            target = value;
            return true;
        };

        if (currentMedia == nullptr) {
            if (assignNonEmpty("ice-ufrag:", description.iceUfrag) ||
                assignNonEmpty("ice-pwd:", description.icePassword)) {
                if (!error.empty()) {
                    description = {};
                    return false;
                }
                continue;
            }
            std::string algorithm;
            std::string fingerprint;
            if (attribute.compare(0, 12, "fingerprint:") == 0) {
                if (!parseFingerprint(attribute.substr(12), algorithm, fingerprint)) {
                    error = "SDP contains an invalid DTLS fingerprint.";
                    description = {};
                    return false;
                }
                description.fingerprintAlgorithm = std::move(algorithm);
                description.fingerprint = std::move(fingerprint);
                continue;
            }
            if (assignNonEmpty("setup:", description.setup)) {
                if (!error.empty()) {
                    description = {};
                    return false;
                }
                continue;
            }
            continue;
        }

        if (assignNonEmpty("mid:", currentMedia->mid) ||
            assignNonEmpty("ice-ufrag:", currentMedia->iceUfrag) ||
            assignNonEmpty("ice-pwd:", currentMedia->icePassword) ||
            assignNonEmpty("setup:", currentMedia->setup)) {
            if (!error.empty()) {
                description = {};
                return false;
            }
            continue;
        }
        if (attribute.compare(0, 12, "fingerprint:") == 0) {
            if (!parseFingerprint(
                    attribute.substr(12),
                    currentMedia->fingerprintAlgorithm,
                    currentMedia->fingerprint)) {
                error = "SDP contains an invalid DTLS fingerprint.";
                description = {};
                return false;
            }
            continue;
        }
        if (attribute.compare(0, 7, "rtpmap:") == 0) {
            if (!parseRtpMap(attribute.substr(7), *currentMedia, error)) {
                description = {};
                return false;
            }
            continue;
        }
        if (attribute.compare(0, 10, "candidate:") == 0) {
            if (currentMedia->iceCandidates.size() >= MaxCandidatesPerMedia) {
                error = "SDP media section contains too many ICE candidates.";
                description = {};
                return false;
            }
            IceCandidate candidate;
            if (!parseIceCandidate("candidate:" + attribute.substr(10), candidate, error)) {
                description = {};
                return false;
            }
            currentMedia->iceCandidates.push_back(std::move(candidate));
        }
    }

    if (description.media.empty()) {
        error = "SDP contains no media sections.";
        description = {};
        return false;
    }

    for (auto& media : description.media) {
        if (media.iceUfrag.empty()) media.iceUfrag = description.iceUfrag;
        if (media.icePassword.empty()) media.icePassword = description.icePassword;
        if (media.fingerprint.empty()) {
            media.fingerprintAlgorithm = description.fingerprintAlgorithm;
            media.fingerprint = description.fingerprint;
        }
        if (media.setup.empty()) media.setup = description.setup;
    }

    return true;
}

} // namespace xcloud
