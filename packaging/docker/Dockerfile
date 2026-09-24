# SPDX-License-Identifier: GPL-3.0-only
#
# melodyd for a server: the engine, the agent and the CLI, without the
# window. Build from the repository root:
#   docker build -f packaging/docker/Dockerfile -t melodyd .

FROM debian:trixie AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config ca-certificates \
        libavformat-dev libavcodec-dev libavutil-dev libswresample-dev libswscale-dev \
        libopenmpt-dev libtag-dev libpipewire-0.3-dev libebur128-dev libutf8proc-dev \
        libsqlite3-dev libcurl4-openssl-dev libssl-dev nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*
COPY . /source
RUN cmake -S /source -B /build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
        -DTRACKKNIFE_BUILD_UI=OFF -DBUILD_TESTING=OFF -DTRACKKNIFE_BUILD_BENCHMARKS=OFF \
        -DTRACKKNIFE_WARNINGS_AS_ERRORS=OFF \
    && cmake --build /build --target trackknife_engine_daemon melody_agent melody_cli \
    && for part in engine agent cli; do DESTDIR=/out cmake --install /build --component $part; done
# The runtime needs the packages these binaries link, and no more: found
# from the binaries themselves rather than kept by hand. ldd names the
# /lib path, dpkg the merged /usr one it was installed under; and a library
# no package owns fails the build rather than the container at start.
RUN for binary in /out/usr/bin/*; do ldd "$binary"; done \
        | awk '/=> \// {print $3}' | sed 's|^/lib/|/usr/lib/|' | sort -u \
        | xargs dpkg -S | cut -d: -f1 | sort -u > /out/runtime-packages \
    && test -s /out/runtime-packages

# Just the binaries and the units, for release downloads:
#   docker buildx build -f packaging/docker/Dockerfile --target binaries \
#       --output type=local,dest=dist .
FROM scratch AS binaries
COPY --from=build /out/usr/ /

FROM debian:trixie-slim
COPY --from=build /out/runtime-packages /tmp/runtime-packages
# PipeWire's client modules and its client.conf are loaded at run time, so
# the binaries do not name them: without them a mounted PipeWire socket
# shows no speakers.
RUN apt-get update && apt-get install -y --no-install-recommends \
        $(cat /tmp/runtime-packages) ca-certificates \
        libpipewire-0.3-modules pipewire-bin \
    && rm -rf /var/lib/apt/lists/* /tmp/runtime-packages \
    && useradd --uid 1000 --create-home melody \
    && mkdir -p /state && chown melody:melody /state
COPY --from=build /out/usr/bin/ /usr/bin/
USER melody
VOLUME /state
# 6603 for clients and agents, 6604 for streams to agents with no copy of
# the music, 5353/udp to be found by name: with host networking, as the
# compose file uses, they need no mapping.
EXPOSE 6603 6604
ENV MELODYD_OPTIONS=""
HEALTHCHECK --interval=30s --timeout=5s \
    CMD melody-cli --server /state/melodyd.sock status > /dev/null || exit 1
ENTRYPOINT ["/bin/sh", "-c", "exec melodyd --state /state --socket /state/melodyd.sock $MELODYD_OPTIONS \"$@\"", "melodyd"]
