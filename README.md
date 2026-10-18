# XCloud Stream for PS4

This project is licensed under GNU GPL version 3. See [LICENSE](LICENSE) and
[third-party notices](THIRD_PARTY_NOTICES.md).

XCloud Stream is a native C++ Xbox Cloud Gaming client for jailbroken PS4 consoles running Orbis OS, packaged as an installable Fake PKG (`.pkg`).

## Current English PS4 build

The maintained English-default XCloud Stream fork, Xbox-inspired dashboard, and
build instructions are in [`nexo-x4-english/`](nexo-x4-english/README.md). The
latest installable PS4 package is
[`XCloud-Stream-v0.1.7-ps4.pkg`](nexo-x4-english/dist/XCloud-Stream-v0.1.7-ps4.pkg);
verify it with the adjacent SHA-256 file before installing.

## Microsoft sign-in

The package uses the public Xbox web client ID and Microsoft consumer OAuth
device-code flow described below. A custom Entra app ID is not used: admin
consent on that app cannot authorize Xbox's console-transfer-token scope.
Tokens saved by earlier builds are tied to the old client and will require a
new phone-based sign-in after installing this build. This Xbox sign-in flow is
undocumented and Microsoft may change or block it.

## Branding

The app uses an original cloud-and-play mark in its interface and PS4 package
icon. The editable vector source is `assets/cloud-stream-icon.svg`; regenerate
the 512x512 package icon with:

```sh
rsvg-convert -w 512 -h 512 assets/cloud-stream-icon.svg \
  -o pkg_root/sce_sys/icon0.png
```

The package build uses this project icon instead of the OpenOrbis sample icon.

### PS4 network diagnostics

To build a temporary package that checks the OpenOrbis UDP socket,
loopback, `fcntl` and `setsockopt` non-blocking alternatives, `poll`, and pipe
support on-console:

```sh
export XCLOUD_NETWORK_DIAGNOSTIC=ON
export XCLOUD_BUILD_DIR="$PWD/build/network-diagnostic"
./build_pkg.sh
```

Launch the diagnostic package on the PS4. The screen shows PASS/FAIL for each
individual socket operation; `[NetworkProbe]` log lines include the return
code and `errno` for failures. The separate build directory keeps the regular
package untouched. It uses a separate PS4 title ID, so it installs alongside
the regular app.
Rebuild normally with `unset XCLOUD_NETWORK_DIAGNOSTIC XCLOUD_BUILD_DIR`
afterward. The probe sends data only over loopback; it does not contact
external hosts.

## Streaming status

The app requests a device code and refresh token from Microsoft's consumer
OAuth v2 endpoint with `xboxlive.signin`, `openid`, `profile`, and
`offline_access`, using the current public Xbox web client ID. It then uses
that refresh token to request the Passport.NET cloud console-transfer-token
scope from `login.live.com`. This Xbox token exchange is undocumented and is
not an Entra app
registration flow; granting admin consent to a custom Entra app does not grant
the Xbox transfer-token scope. Saved tokens tied to the previous client ID are
rejected and require signing in again.

After sign-in, the app authenticates with Xbox and attempts the observed
xCloud service login and title-catalog request, following string continuation
tokens across pages, and reports the number of catalog entries returned.
These service endpoints are undocumented and unsupported for third-party
clients; their behavior can change without notice. The app also attempts to
start a selected title, exchange the transfer token, negotiate SDP and ICE,
and complete the WebRTC data-channel handshake. An experimental PS4 hardware
AVC decoder is now wired to the video path. It still needs validation on a
console; Opus audio decoding is not implemented.

When Fortnite's product ID appears in the returned catalog, pressing the
DualShock 4 Cross button requests a cloud session and reports its queue and
provisioning state. The client does not locally gate this on catalog
entitlement flags; the cloud service decides whether the account can start it.
At `ReadyToConnect`, the app exchanges its Xbox MSA refresh token for an Xbox
Live console-transfer token using the Passport.NET
`PURPOSE_XBOX_CLOUD_CONSOLE_TRANSFER_TOKEN` scope, then submits that token to
the session connect endpoint. The transfer token is kept in memory and is
never logged or displayed.

`H264RtpDepacketizer` parses received RTP headers and reconstructs H.264 access
units from single-NAL, STAP-A, and FU-A packets. libdatachannel handles ICE,
DTLS-SRTP, and media transport. The PS4 `sceVideodec` hardware AVC path feeds
decoded NV12 frames to the display; this experimental path requires on-console
validation. Opus audio decoding and a loss-recovery jitter buffer remain open.

`parseSessionDescription` is an additional WebRTC groundwork component. It
extracts media sections, ICE credentials/candidates, DTLS fingerprints, setup
roles, payload types, and RTP codec mappings. ICE candidates are validated and
exposed as typed host, server-reflexive, peer-reflexive, or relay endpoints.
This parser is separate from the live SDP and ICE exchange used by the stream.

### Experimental WebRTC transport probe

The project has an opt-in, pinned libdatachannel build for OpenOrbis. To fetch
the exact dependency revisions and build a separate diagnostic package:

```sh
git submodule update --init --recursive
XCLOUD_WEBRTC_DIAGNOSTIC=ON \
XCLOUD_BUILD_DIR="$PWD/build/webrtc-diagnostic" \
./build_pkg.sh
```

The diagnostic package has a separate title ID, so it can be installed beside
the regular app. It creates a local libdatachannel peer and data channel,
generates SDP, and waits up to 20 seconds for local ICE gathering. The screen
reports whether gathering completed and how many local candidates were found.
It does not contact external hosts.
Errors identify which initialization stage failed.

This cross-compiles libdatachannel, libjuice, usrsctp, libSRTP, and Mbed TLS.
Small platform patches are applied during CMake configuration: libjuice uses
the tested Orbis `SO_NBIO` socket option, libdatachannel avoids the unavailable
thread-local destructor runtime, unsupported FreeBSD-only headers/APIs are
bypassed, and usrsctp reads entropy from `/dev/urandom`. The regular app remains
independent of these submodules unless WebRTC is enabled.

The probe has not yet been run on-console. Passing it would verify local ICE
gathering only; it would not establish a remote DTLS-SRTP session, perform
xCloud signaling, receive media, or decode the PS4 stream. Preserve upstream
licenses and notices when distributing a build that includes these libraries.

Microsoft's GDK streaming APIs are
intended for game developers and private offerings, not for starting consumer
xCloud sessions. See Microsoft's [game-streaming
overview](https://learn.microsoft.com/en-us/gaming/gdk/docs/features/common/game-streaming/game-streaming-overview?view=gdk-2604)
and [XGameStreaming API reference](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xgamestreaming/xgamestreaming_members?view=gdk-2604).
The installed OpenOrbis `Videodec.h` exposes only untyped function declarations.
The current backend uses the typed ABI definitions from the OpenOrbis
VideodecX header change and should be treated as experimental until tested on
PS4 hardware. `convertNv12ToRgba` converts decoded frames for display.

### H.264 RTP depacketizer tests

The host-side unit test can be compiled and run without PS4 libraries:

```sh
c++ -std=c++17 -Wall -Wextra -Werror -Iinclude \
  src/h264_rtp.cpp tests/h264_rtp_test.cpp -o /tmp/h264_rtp_test
/tmp/h264_rtp_test
```

The SDP parser tests can also be run on the host:

```sh
c++ -std=c++17 -Wall -Wextra -Werror -Iinclude \
  src/webrtc_ice.cpp src/webrtc_sdp.cpp tests/webrtc_sdp_test.cpp \
  -o /tmp/webrtc_sdp_test
/tmp/webrtc_sdp_test
```

The STUN Binding Success Response parser can be tested on the host:

```sh
c++ -std=c++17 -Wall -Wextra -Werror -Iinclude \
  src/stun.cpp tests/stun_test.cpp -o /tmp/stun_test
/tmp/stun_test
```

This parser validates the response structure, transaction ID, and IPv4
`XOR-MAPPED-ADDRESS`; it does not verify STUN message integrity or fingerprints,
send ICE checks, or establish a media connection. Do not trust its parsed
address for a live ICE connection until cryptographic verification is added.

NV12 frame conversion tests can be run on the host:

```sh
c++ -std=c++17 -Wall -Wextra -Werror -Iinclude \
  src/nv12_converter.cpp tests/nv12_converter_test.cpp -o /tmp/nv12_converter_test
/tmp/nv12_converter_test
```

On the host, the PS4-only decoder backend remains unavailable; this check
verifies it fails explicitly rather than reporting success without a frame:

```sh
c++ -std=c++17 -Wall -Wextra -Werror -Iinclude \
  src/video_decoder.cpp tests/video_decoder_test.cpp -o /tmp/video_decoder_test
/tmp/video_decoder_test
```

The RGBA frame renderer scales decoded frames into the display area while
preserving their aspect ratio:

```sh
c++ -std=c++17 -Wall -Wextra -Werror -Iinclude \
  src/frame_renderer.cpp tests/frame_renderer_test.cpp -o /tmp/frame_renderer_test
/tmp/frame_renderer_test
```

ICE candidate parsing tests can be run on the host:

```sh
c++ -std=c++17 -Wall -Wextra -Werror -Iinclude \
  src/webrtc_ice.cpp tests/webrtc_ice_test.cpp -o /tmp/webrtc_ice_test
/tmp/webrtc_ice_test
```

## Directory Structure

```
ps4-xcloud/
├── CMakeLists.txt          # Main build configuration (OpenOrbis toolchain)
├── Makefile                # Optional OpenOrbis Makefile
├── include/                # Application headers
│   ├── auth.hpp            # Microsoft OAuth & Xbox Live token handling
│   ├── h264_rtp.hpp        # RTP to H.264 access-unit depacketizer
│   ├── webrtc_sdp.hpp      # SDP attributes needed for WebRTC negotiation
│   ├── xcloud_session.hpp  # xCloud service login and catalog
│   ├── video_decoder.hpp   # Experimental PS4 hardware AVC decoder interface
│   └── pad_input.hpp       # DualShock 4 input mapping
├── src/                    # Application sources
│   ├── main.cpp            # App entrypoint & UI
│   ├── auth.cpp
│   ├── h264_rtp.cpp
│   ├── xcloud_session.cpp
│   ├── video_decoder.cpp
│   └── pad_input.cpp
├── tests/
│   └── h264_rtp_test.cpp   # Host-side depacketizer tests
└── sce_sys/                # PS4 Package metadata
    ├── param.sfo           # Title metadata (Title ID, Title, Category)
    ├── icon0.png           # 512x512 app icon
    └── pic0.png            # 1920x1080 splash screen
```
