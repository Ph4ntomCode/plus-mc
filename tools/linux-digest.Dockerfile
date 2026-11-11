# +MC -- the Linux-gcc toolchain for the container checks
# (`tools/check_column_digests_linux.sh`, `tools/check_light_seam_linux.sh`):
# ubuntu 24.04 gcc for linux/amd64.
#
# Ubuntu 24.04 is the gcc the ubuntu-gcc CI runner and the 22 Oct platform probe
# used (gcc 13.3), and the apt packages are the ones `ci.yml` installs for it.
# Pinning the distro here rather than pulling `gcc:latest` is the point: the
# digests and the seam are cross-compiler statements, so the compiler has to be
# the recorded one. The scripts tag this image with this file's hash
# (`plusmc-linux-toolchain:deps-*`), so a repeat run reuses it without touching
# the network and an edit here can never silently reuse an old image.
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build zlib1g-dev libssl-dev libcurl4-openssl-dev \
 && rm -rf /var/lib/apt/lists/*
