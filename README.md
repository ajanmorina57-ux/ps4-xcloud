# xCloud-PS4: Xbox Cloud Gaming Native Port for PlayStation 4

A native C++ Xbox Cloud Gaming (xCloud) client for jailbroken PS4 consoles running Orbis OS, packaged as an installable Fake PKG (`.pkg`).

## Microsoft sign-in setup

Sign-in requires an application registration that you control. Do not use another app's client ID.

1. In the [Microsoft Entra admin center](https://entra.microsoft.com/), open **Entra ID > App registrations > New registration**.
2. Choose **Personal Microsoft accounts only** as the supported account type, then register the app.
3. In the app's **Authentication > Advanced settings**, enable **Allow public client flows**.
4. Copy the **Application (client) ID** from the app overview.
5. Build with that ID:

   ```sh
   export XCLOUD_CLIENT_ID="your-application-client-id"
   ./build_pkg.sh
   ```

   Or pass `-DXCLOUD_CLIENT_ID="your-application-client-id"` when configuring CMake directly.

The client ID is a public identifier, not a client secret. The app will leave sign-in disabled when no client ID is configured.

## Streaming status

The app supports Microsoft device-code sign-in and Xbox token authentication.
It now also attempts the observed xCloud service login and title-catalog
request, following string continuation tokens across pages, and reports the
number of catalog entries returned. These service endpoints are undocumented
and unsupported for third-party clients; their
behavior can change without notice. Session provisioning is only a connectivity
test; the client does not yet connect to the stream or display audio/video.

When Fortnite's product ID appears in the returned catalog, pressing the
DualShock 4 Cross button requests a cloud session and reports its queue and
provisioning state. The client does not locally gate this on catalog
entitlement flags; the cloud service decides whether the account can start it.
At `ReadyToConnect`, the app requests a second Microsoft device-code approval
for the Xbox Live console-transfer token using the Passport.NET
`PURPOSE_XBOX_CLOUD_CONSOLE_TRANSFER_TOKEN` scope, submits that token to the
session connect endpoint, then closes the test session. The transfer token is
kept in memory and is never logged or displayed. This handoff is still not
gameplay: the client does not yet implement WebRTC transport or receive
audio/video.

The current Microsoft app registration rejects that console-transfer scope as
not pre-authorized. Catalog loading and session provisioning work, but the
connect handoff cannot proceed unless Microsoft authorizes the scope for this
client. Do not substitute another application's client ID or reuse its tokens.

The first media building block is `H264RtpDepacketizer`. It parses RTP headers
and reconstructs H.264 access units from single-NAL, STAP-A, and FU-A packets.
It expects already-authenticated/decrypted RTP payloads; UDP transport, jitter
buffering, DTLS/SRTP, WebRTC signaling, and stream discovery are not implemented.

`parseSessionDescription` is an additional WebRTC groundwork component. It
extracts media sections, ICE credentials/candidates, DTLS fingerprints, setup
roles, payload types, and RTP codec mappings. ICE candidates are validated and
exposed as typed host, server-reflexive, peer-reflexive, or relay endpoints.
This does not negotiate SDP or establish a WebRTC connection.

The PS4 AVC decoder is also not implemented. Microsoft's GDK streaming APIs are
intended for game developers and private offerings, not for starting consumer
xCloud sessions. See Microsoft's [game-streaming
overview](https://learn.microsoft.com/en-us/gaming/gdk/docs/features/common/game-streaming/game-streaming-overview?view=gdk-2604)
and [XGameStreaming API reference](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xgamestreaming/xgamestreaming_members?view=gdk-2604).
The current OpenOrbis `Videodec.h` exposes only untyped function declarations,
without the decoder structures required to implement the PS4 backend safely.
`convertNv12ToRgba` is tested frame-output groundwork, not a decoder or
standalone stream player.

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

NV12 frame conversion tests can be run on the host:

```sh
c++ -std=c++17 -Wall -Wextra -Werror -Iinclude \
  src/nv12_converter.cpp tests/nv12_converter_test.cpp -o /tmp/nv12_converter_test
/tmp/nv12_converter_test
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
│   ├── video_decoder.hpp   # Video decoder interface (PS4 backend pending)
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
