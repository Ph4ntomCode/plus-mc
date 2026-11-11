# +MC -- the aarch64 toolchain for the container checks' `--arch arm64` legs
# (`tools/check_column_digests_linux.sh`, `tools/check_light_seam_linux.sh`),
# the emulated leg of the decorated-column and light-seam checks.
#
# Two stages, built by target:
#
#   build  the amd64 userspace that cross-compiles the probe with Ubuntu's
#          aarch64 gcc. The arm64 packages come from ports.ubuntu.com, and the
#          main archive is pinned to amd64 so both can be installed at once
#          (`zlib1g-dev:arm64`, `libssl-dev:arm64`). The stage runs on
#          $BUILDPLATFORM, so on an arm64 host the cross compiler is native too.
#
#   run    the arm64 runtime the probes need. It installs nothing: Ubuntu's
#          base image already carries the libstdc++, zlib and libssl they link,
#          and the stage asserts exactly that. The scripts run the cross-built
#          binaries here with `--platform linux/arm64`: native on an arm64 host,
#          qemu-emulated on x86_64. That emulation is why these legs are slow
#          (72 minutes for the seam's 75 columns, ~13 for the digest's), not why
#          they are checked.
#
# The scripts tag both stages with this file's hash
# (`plusmc-linux-toolchain:arm64-{cross,run}-*`), so a repeat run reuses them
# without touching the network and an edit here builds a new tag rather than
# silently reusing an old image.
FROM --platform=$BUILDPLATFORM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN dpkg --add-architecture arm64 \
 && sed -i 's/^Types: deb$/Types: deb\nArchitectures: amd64/' /etc/apt/sources.list.d/ubuntu.sources \
 && printf 'Types: deb\nURIs: http://ports.ubuntu.com/ubuntu-ports/\nSuites: noble noble-updates noble-security\nComponents: main universe restricted multiverse\nArchitectures: arm64\nSigned-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\n' > /etc/apt/sources.list.d/arm64.sources \
 && apt-get update -qq \
 && apt-get install -y --no-install-recommends \
      cmake ninja-build g++-aarch64-linux-gnu zlib1g-dev:arm64 libssl-dev:arm64 \
 && rm -rf /var/lib/apt/lists/*

FROM --platform=linux/arm64 ubuntu:24.04 AS run
# Nothing to install: the probe links the cross toolchain's libstdc++, zlib and
# libssl, and this image already carries all three -- asserted here so a base
# image that dropped one fails at build time rather than at the probe. An
# `apt-get install` would only add a dependency on the arm64 archive's index
# being reachable, which is the flakiest part of this leg and irrelevant to a
# stage that builds nothing.
RUN dpkg-query -W -f='${Package} ${Version}\n' libstdc++6 zlib1g libssl3t64
