# arm64 build and software-GPU test environment. Debian trixie = the base of
# Raspberry Pi OS (trixie), so binaries built here run on the Pi's userland.
# Mesa here provides lavapipe (Vulkan) and llvmpipe (EGL/GLES) for functional
# runs without the Pi; performance numbers come only from the Pi itself.
FROM --platform=linux/arm64 debian:trixie

COPY packages.txt /tmp/packages.txt
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      $(grep -v '^#' /tmp/packages.txt) \
    && rm -rf /var/lib/apt/lists/*

ENV PREFIX=/opt/dv \
    PKG_CONFIG_PATH=/opt/dv/lib/pkgconfig:/opt/dv/lib/aarch64-linux-gnu/pkgconfig \
    LD_LIBRARY_PATH=/opt/dv/lib:/opt/dv/lib/aarch64-linux-gnu
WORKDIR /work
