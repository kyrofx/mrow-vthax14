# Development/build environment for BiteDJ, in two flavours selected by
# build args (see compose.yaml):
#
#   pi      debian:trixie, tuned for the Cortex-A72. Matches the shipping
#           device: Raspberry Pi 4B on Raspberry Pi OS 64-bit (trixie), so the
#           binary links against the same glibc/Qt versions.
#   ubuntu  ubuntu:24.04, untuned. Matches CI (.github/workflows/tests.yml).
#
# Both are arm64-only on Apple Silicon hosts (native); on x86 hosts the pi
# flavour is emulated (compose.yaml pins its platform), which is slow.
#
# The source tree is not baked into the image; it is bind-mounted at /src.
ARG BASE_IMAGE=debian:trixie
FROM ${BASE_IMAGE}

ARG DEBIAN_FRONTEND=noninteractive
ARG CPU_FLAGS=""

# fonts-ubuntu (installed by debian_buildenv.sh) lives in Debian's non-free;
# Raspberry Pi OS enables the same components. No-op on Ubuntu.
RUN if [ -f /etc/apt/sources.list.d/debian.sources ]; then \
        sed -i 's/^Components: main$/Components: main contrib non-free non-free-firmware/' \
            /etc/apt/sources.list.d/debian.sources; \
    fi

# debian_buildenv.sh calls sudo, and the build needs git for `git describe`.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        git \
        ninja-build \
        sudo \
    && rm -rf /var/lib/apt/lists/*

COPY tools/debian_buildenv.sh /tmp/debian_buildenv.sh
# `< /dev/null` so the script never blocks on its interactive fonts prompt.
RUN /tmp/debian_buildenv.sh setup < /dev/null \
    && rm -rf /var/lib/apt/lists/* /tmp/debian_buildenv.sh

# Virtual display served over VNC for running the app (docker/run-vnc.sh).
# Mesa's software renderer stands in for a GPU.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        libgl1-mesa-dri \
        x11-utils \
        x11vnc \
        xvfb \
    && rm -rf /var/lib/apt/lists/* \
    && install -d -m 1777 /tmp/.X11-unix

# Build as an unprivileged user so files written to the bind-mounted source
# tree are not root-owned. ubuntu:24.04 ships an `ubuntu` user holding UID
# 1000; drop it so both flavours use the same user and paths.
ARG UID=1000
RUN (userdel --remove ubuntu 2>/dev/null || true) \
    && useradd --create-home --uid "${UID}" --shell /bin/bash builder \
    && echo 'builder ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/builder \
    && git config --system --add safe.directory '*' \
    && mkdir -p /src/build /home/builder/.cache/ccache /home/builder/.bitedj \
    && chown -R builder:builder /src /home/builder

# CMake reads CFLAGS/CXXFLAGS on first configure.
ENV CFLAGS="${CPU_FLAGS}" \
    CXXFLAGS="${CPU_FLAGS}" \
    CCACHE_DIR=/home/builder/.cache/ccache \
    CMAKE_GENERATOR=Ninja \
    QT_QPA_PLATFORM=offscreen

USER builder
WORKDIR /src
CMD ["bash"]
