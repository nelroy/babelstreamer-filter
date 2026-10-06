# Third-party licenses

`babelstreamer-filter` is licensed GPL-2.0-or-later (see [LICENSE](LICENSE)). It
builds against or bundles the following third-party software.

## Bundled in the plugin

### Silero VAD model (`data/ggml-silero-v6.2.0.bin`)

MIT License. Copyright (c) 2020-present, Silero Team.
Source: <https://github.com/snakers4/silero-vad>

GGML conversion (weights unmodified) via whisper.cpp's own
`convert-silero-vad-to-ggml.py`; this copy was fetched pre-converted from
<https://huggingface.co/ggml-org/whisper-vad>.

## Statically linked

### whisper.cpp

MIT License. Copyright (c) 2023-2024 The ggml authors.
Source: <https://github.com/ggml-org/whisper.cpp>

Linked as a static library, built from a separate whisper.cpp checkout (see
`WHISPER_ROOT` in [CMakeLists.txt](CMakeLists.txt)). Not vendored in this
repository.

## Dynamically linked at runtime

These resolve against the copies already bundled with the host OBS Studio
installation, so this plugin does not redistribute them, with one
exception noted below.

### Qt 6 (Core, Widgets, Network)

GNU Lesser General Public License v3 (LGPL-3.0), or the Qt Commercial
License. Copyright (C) The Qt Company Ltd and contributors.
Source: <https://www.qt.io/>, <https://code.qt.io/cgit/>

Core, Widgets and Network link dynamically against the Qt 6 build shipped
inside OBS Studio itself. 

`data/tls/libqsecuretransportbackend.dylib` and
`data/tls/qschannelbackend.dll` are the exception. These are unmodified,
pre-built Qt TLS backend plugin binaries, and this plugin does bundle and
redistribute them (see `src/TlsBackend.hpp`). OBS's own Qt deployment
strips every TLS backend out, and without one, `QNetworkAccessManager`
can't make HTTPS requests. Both files are unmodified copies from the
official Qt 6 distribution; the corresponding source for the exact
version shipped is available from Qt's own repository above.

### Vulkan Loader (Windows only)

Apache License 2.0. Copyright (c) The Khronos Group Inc.
Source: <https://github.com/KhronosGroup/Vulkan-Loader>

Used for the Windows build's GPU acceleration backend when selected
(`WHISPER_HAS_VULKAN=1`). 

### mbedTLS

Dual-licensed Apache License 2.0 or GPL-2.0-or-later, at the recipient's
choice. Copyright The Mbed TLS Contributors.
Source: <https://github.com/Mbed-TLS/mbedtls>

Used for the `wss://` (TLS) path in `src/websocket-client.cpp`. Linked
dynamically against the copy OBS Studio's own `obs-outputs` module
already bundles for RTMPS. This plugin does not vendor or redistribute
mbedTLS itself.

## Not linked (reference only)

`src/sha256.cpp`/`.h` is a from-scratch SHA-256 implementation adapted
from Brad Conte's public-domain reference (<https://github.com/B-Con/crypto-algorithms>). 
