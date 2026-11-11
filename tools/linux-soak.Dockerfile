# +MC -- the server's soak scenes under Linux gcc, in a container.
#
# `tools/soak_linux.sh` mounts the checkout at /src, builds `plusmc` with this
# image and runs `bench/soak.py` with server and bots on the container's own
# loopback, so the four vCPUs are shared exactly as they are on the host.
#
# It is deliberately a *separate* image from `linux-digest.Dockerfile`: that one
# is the pinned gcc for the content checks (the column digests and the light
# seam), whose image tags are the toolchain's own hash. Adding python3 here
# instead of there means those tags do not move. What this image adds is only
# python3 -- the harness itself -- on top of the same ubuntu 24.04 toolchain.
FROM --platform=linux/amd64 ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build zlib1g-dev libssl-dev libcurl4-openssl-dev \
      python3 \
 && rm -rf /var/lib/apt/lists/*
