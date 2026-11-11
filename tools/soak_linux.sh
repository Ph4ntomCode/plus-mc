#!/usr/bin/env bash
# +MC -- a soak scene under Linux gcc, in a container.
#
# `bench/soak.py` measures whatever binary this machine built. Item 9 of
# docs/PHASE5.md owes the *same scenes* on the second platform; this runs them
# the way the first Linux arm was run: the checkout is mounted at /src, `plusmc`
# is built there with ubuntu 24.04's gcc, and server and bots share the
# container's loopback and its vCPUs.
#
#   tools/soak_linux.sh --json bench/baseline/linux-20bots-void-10s.json \
#     -- --bots 20 --seconds 10 --view-distance 8
#
# With no `--` it takes the long generated-world cell item 4 owes: 20 stationary
# bots on a 256-block grid, view distance 8, seed 42, three hours.
#
# What the record cannot see: the container has its own kernel, so its
# /proc/loadavg and its CPU-idle sample are the VM's, and its `busy` verdict is
# not the host's (docs/PHASE5.md §4, "The same scene on Linux"). Paired arms on
# the same container are still the fair comparison.
#
# The checkout is mounted, not streamed: `data/vanilla` is generated data and is
# not tracked, and the server needs it. The image is tagged with its Dockerfile's
# hash, so a repeat run reuses it; it is a separate image from the digest/seam
# toolchain because it also carries python3 for the harness.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
tree="$root"
json=""
keep=0
image_override=""
have_soak_args=0
soak_args=()

die() {
  printf '%s\n' "$*" >&2
  exit 1
}

usage() {
  printf 'usage: %s --json FILE [--tree DIR] [--image TAG] [--keep] [-- SOAK ARGS]\n' "${BASH_SOURCE[0]}"
  printf '  FILE       where to write the record (required)\n'
  printf '  DIR        checkout to measure (default: the tree this script is in)\n'
  printf '  TAG        an existing toolchain image to use instead of building\n'
  printf '             linux-soak.Dockerfile (it must carry g++, cmake, ninja and\n'
  printf '             python3; the escape hatch for a host whose Docker cannot\n'
  printf '             reach a distro package index)\n'
  printf '  --keep     keep the work directory (the server log, the raw record)\n'
  printf '  SOAK ARGS  passed to bench/soak.py; omitted, the default is the long\n'
  printf '             generated-world cell (20 bots, 3 h, seed 42, spread 256, vd8)\n'
}

while [ $# -gt 0 ]; do
  case "$1" in
    --json)
      [ $# -ge 2 ] || die "--json takes a file"
      json="$2"
      shift 2
      ;;
    --json=*)
      json="${1#*=}"
      shift
      ;;
    --tree)
      [ $# -ge 2 ] || die "--tree takes a directory"
      tree="$2"
      shift 2
      ;;
    --tree=*)
      tree="${1#*=}"
      shift
      ;;
    --image)
      [ $# -ge 2 ] || die "--image takes a tag"
      image_override="$2"
      shift 2
      ;;
    --image=*)
      image_override="${1#*=}"
      shift
      ;;
    --keep)
      keep=1
      shift
      ;;
    --)
      shift
      [ $# -gt 0 ] || die "no soak arguments after --"
      have_soak_args=1
      soak_args=("$@")
      break
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      printf 'unknown argument %s\n' "$1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

[ -n "$json" ] || die "no --json FILE: a measurement must name its record"
[ -d "$tree" ] || die "no such directory: $tree"
[ -f "$tree/CMakeLists.txt" ] || die "$tree is not the +MC tree (no CMakeLists.txt)"
[ -f "$tree/bench/soak.py" ] || die "$tree has no bench/soak.py"
command -v docker >/dev/null 2>&1 || die "docker is not installed -- this run needs Docker"
# `docker version` with a format string, not `docker info`: info can block on a
# half-started daemon (it has on this box), while the server version is the one
# question this line needs answered.
docker version --format '{{.Server.Version}}' >/dev/null 2>&1 \
  || die "the Docker daemon is not reachable -- start Docker Desktop (or dockerd) and run this again"
git -C "$tree" rev-parse --is-inside-work-tree >/dev/null 2>&1 \
  || die "$tree is not a git checkout"

if [ "$have_soak_args" -eq 0 ]; then
  soak_args=(--bots 20 --seconds 10800 --stationary --spread 256 \
             --view-distance 8 --generated 42)
fi

invoked="$(pwd)"
case "$json" in
  /*) ;;
  *) json="$invoked/$json" ;;
esac
mkdir -p "$(dirname "$json")"

dockerfile="$here/linux-soak.Dockerfile"
[ -f "$dockerfile" ] || die "missing $dockerfile"
# The Linux arm this records is the x86_64 gcc leg; pin it so the same scene can
# be reproduced on an arm64 host under emulation rather than silently changing
# platforms (the column-digest check does the same).
platform="linux/amd64"

dockerfile_hash() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | cut -c1-12
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | cut -c1-12
  else
    die "no sha256sum or shasum -- needed to tag the toolchain image"
  fi
}

if [ -n "$image_override" ]; then
  image="$image_override"
  docker image inspect "$image" >/dev/null 2>&1 \
    || die "no image $image (docker image ls)"
  printf 'toolchain %s (given with --image)\n' "$image"
else
  image="plusmc-linux-soak:deps-$(dockerfile_hash "$dockerfile")"
  if docker image inspect "$image" >/dev/null 2>&1; then
    printf 'toolchain %s\n' "$image"
  else
    printf 'building toolchain %s (first run for this Dockerfile)\n' "$image"
    docker build --quiet --platform "$platform" -f "$dockerfile" -t "$image" "$here" >/dev/null
  fi
fi

tmpdir="${TMPDIR:-/tmp}"
work="$(mktemp -d "${tmpdir%/}/plusmc-linux-soak.XXXXXX")"
record="$work/record.json"
server_log="$work/server.log"
printf '%s\n' "${soak_args[@]}" > "$work/args.txt"

cleanup() {
  local exit_status=$?
  trap - EXIT
  if [ "$exit_status" -ne 0 ]; then
    printf '\nrun failed; the work directory is kept at %s\n' "$work" >&2
  elif [ "$keep" -eq 1 ]; then
    printf '\nwork directory kept at %s (server log %s)\n' "$work" "$server_log"
  else
    rm -rf "$work"
  fi
  exit "$exit_status"
}
trap cleanup EXIT

printf 'building plusmc from %s and running bench/soak.py in the container\n' "$tree"
docker run --rm -i --platform "$platform" -v "$tree:/src:ro" -v "$work:/out" "$image" bash -euc '
  arch="$(uname -m)"
  test "$arch" = x86_64 || { echo "container arch is $arch, not x86_64" >&2; exit 1; }
  cmake -S /src -B /tmp/build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    -DCMAKE_CXX_COMPILER=g++ >&2
  cmake --build /tmp/build --target plusmc >&2
  mapfile -t soak_args < /out/args.txt
  cd /tmp
  python3 /src/bench/soak.py --bin /tmp/build/src/plusmc \
    --json /out/record.json --log /out/server.log "${soak_args[@]}" >&2
'

[ -s "$record" ] || die "the container produced no record"
cp "$record" "$json"

python3 - "$json" <<'PY'
import json
import sys
d = json.load(open(sys.argv[1]))
loaded = d.get("loaded", {})
rss = d.get("rss_bytes", {})
chunks = d.get("chunks", {})
print(f"record {sys.argv[1]}")
print(f"  bots {d.get('bots')}  seconds {d.get('seconds')}  windows {loaded.get('tps', {}).get('n')}"
      f"  players {loaded.get('players', {}).get('min')}-{loaded.get('players', {}).get('max')}")
print(f"  TPS avg {loaded.get('tps', {}).get('avg')}  min {loaded.get('tps', {}).get('min')}"
      f"  work avg {loaded.get('work_avg_ms')} ms  peak {loaded.get('work_peak_ms')} ms")
print(f"  chunks distinct {chunks.get('distinct')}  sends {chunks.get('received')}"
      f"  RSS first/last/peak MB {rss.get('first', 0) / 1e6:.1f} / {rss.get('last', 0) / 1e6:.1f}"
      f" / {rss.get('peak', 0) / 1e6:.1f}")
PY
