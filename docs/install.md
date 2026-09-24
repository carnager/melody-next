# Installation

## Arch Linux

From the AUR:

- `trackknife-git`: the desktop app
- `melody-git`: the engine (`melodyd`), `melody-agent` and `melody-cli`. On a
  server you only need `melodyd-git`.

Or build the same packages from this repository:

```sh
cd packaging/arch
makepkg -si
```

## Debian 13

Each [release](https://github.com/carnager/melody-next/releases/latest) has
`melodyd`, `melody-agent` and `melody-cli` built for Debian 13, for amd64 and
arm64. They need these libraries:

```sh
sudo apt install libavcodec61 libavformat61 libavutil59 libswresample5 \
    libswscale8 libopenmpt0t64 libtag2 libpipewire-0.3-0t64 \
    libpipewire-0.3-modules libsqlite3-0 libcurl4t64 libssl3t64 libutf8proc3
```

Unpack the archive, copy the binaries to `/usr/bin`, and the `.service` files
to `~/.config/systemd/user/` if you want systemd to run them.

The desktop app isn't in the release yet; build it from source.

## Docker

For a server there's an image with the engine:
`ghcr.io/carnager/melodyd:latest`. The compose file in
[`packaging/docker/`](../packaging/docker/docker-compose.yml) shows how to
run it. Change the music path and the name, then:

```sh
docker compose -f packaging/docker/docker-compose.yml up -d
```

## Android

Download the APK from the
[latest release](https://github.com/carnager/melody-next/releases/latest)
and open it on the phone. Android asks you to allow installs from your
browser or file manager first.

## From source

You need a C++23 compiler, CMake ≥ 3.28, Ninja, pkg-config, and:

- Qt ≥ 6.4 (Widgets, Concurrent, DBus, Network, Test)
- FFmpeg ≥ 6 with swscale, TagLib ≥ 2.0, libopenmpt ≥ 0.7
- PipeWire ≥ 0.3.50, libebur128 ≥ 1.2, SQLite ≥ 3.37, libutf8proc ≥ 2.9
- libcurl, OpenSSL (libcrypto), nlohmann-json
- optional: Chromaprint's `fpcalc`, for AcoustID

```sh
cmake --preset release
cmake --build --preset release
./build/release/src/bench/trackknife
```

The Android app is in `android/`:

```sh
cd android && ./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

Next: [Setup](setup.md).
