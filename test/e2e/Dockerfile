# Build, test and playback image for owe. test/e2e/run.sh builds it for
# linux/arm64 by default and mounts the source tree at /src.
FROM debian:trixie

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential meson ninja-build pkgconf cmake \
        libwayland-dev wayland-protocols libegl-dev libgles-dev libgl-dev libepoxy-dev \
        libmpv-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
        libsystemd-dev qt6-declarative-dev \
        python3 socat ffmpeg \
        sway grim libegl-mesa0 libgl1-mesa-dri \
    && rm -rf /var/lib/apt/lists/*

# sway and owed run as an ordinary user, as in a desktop session.
RUN useradd --create-home --uid 1000 owe
USER owe
WORKDIR /home/owe

ENTRYPOINT ["/bin/sh", "/src/test/e2e/check.sh"]
