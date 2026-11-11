#!/usr/bin/env bash
# +MC -- the generation-time light seam under Linux gcc, in a container.
#
# `bench/seam_track.py` measures the seam with whatever binary this machine has.
# This is the same measurement on the platform the record is a statement about:
# the tree is streamed into `ubuntu:24.04`, `plusmc-light-seam` is built there
# with gcc 13.3.0 (the ubuntu-gcc runner's toolchain) and run on the grid the
# record was cut on, and the measurement is compared with the pin
# `.github/workflows/ci.yml` names as `SEAM_BASELINE`:
#
#   tools/check_light_seam_linux.sh                 # linux/amd64, native
#   tools/check_light_seam_linux.sh --arch arm64    # aarch64 gcc, qemu-run
#
# `--arch arm64` is the leg docs/PHASE5.md's platform check records: the probe is
# cross-compiled with Ubuntu's aarch64 gcc (`tools/linux-digest-arm64.Dockerfile`,
# shared with the column-digest check) and run in an arm64 container -- native on
# an arm64 host, qemu-emulated on x86_64. That emulation is why the leg is the
# slow one: 4,307 s (72 minutes) for the 75-column grid on this box, against 52 s
# native, and against ~13 minutes for the column digest's 75 columns.
#
# `--tree DIR` measures another git checkout instead of this one. `--record FILE`
# compares against a record other than the workflow's pin -- pointing it at a
# one-column record is a two-minute way to exercise a leg end to end.
#
# The toolchain images are tagged with the Dockerfile's own hash, so a repeat run
# reuses the image without touching the network (a cold BuildKit cache cannot
# force an apt fetch) while an edited Dockerfile builds a new tag instead of
# silently reusing the old image. The checkout is streamed in, never mounted, and
# the measurement is written to the run's temporary directory; on failure it is
# kept and its path printed, so a moved field can be read without a second run.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
record_override=""
platform="linux/amd64"
dockerfile="$here/linux-digest.Dockerfile"
tree="$root"
arch="amd64"

die() {
  printf '%s\n' "$*" >&2
  exit 1
}

usage() {
  printf 'usage: %s [--arch amd64|arm64] [--tree DIR] [--record FILE]\n' "${BASH_SOURCE[0]}"
  printf '  ARCH  amd64 (default, native gcc) or arm64 (cross-compiled, emulated)\n'
  printf '  DIR   checkout to measure (default: the tree this script is in)\n'
  printf '  FILE  record to compare with (default: SEAM_BASELINE in ci.yml)\n'
}

while [ $# -gt 0 ]; do
  case "$1" in
    --arch)
      [ $# -ge 2 ] || die "--arch takes amd64 or arm64"
      arch="$2"
      shift 2
      ;;
    --arch=*)
      arch="${1#*=}"
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
    --record)
      [ $# -ge 2 ] || die "--record takes a file"
      record_override="$2"
      shift 2
      ;;
    --record=*)
      record_override="${1#*=}"
      shift
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

case "$arch" in
  amd64)
    ;;
  arm64)
    platform="linux/arm64"
    dockerfile="$here/linux-digest-arm64.Dockerfile"
    ;;
  *)
    die "--arch takes amd64 or arm64, not $arch"
    ;;
esac

# The pin is whatever CI compares against, read from the workflow rather than
# copied here: a re-recorded baseline is one edit there, and this check cannot
# drift away from the job it stands in for.
if [ -n "$record_override" ]; then
  record="$record_override"
else
  record="$(sed -n 's/^[[:space:]]*SEAM_BASELINE:[[:space:]]*//p' \
    "$root/.github/workflows/ci.yml" | head -1 | tr -d '\r')"
  [ -n "$record" ] || die "no SEAM_BASELINE in $root/.github/workflows/ci.yml"
  record="$root/$record"
fi

[ -d "$tree" ] || die "no such directory: $tree"
[ -f "$tree/CMakeLists.txt" ] || die "$tree is not the +MC tree (no CMakeLists.txt)"
[ -f "$record" ] || die "missing the record $record"
[ -f "$dockerfile" ] || die "missing $dockerfile"
command -v docker >/dev/null 2>&1 || die "docker is not installed -- this check needs Docker"
command -v python3 >/dev/null 2>&1 || die "python3 is not installed -- needed for the comparison"
docker info >/dev/null 2>&1 \
  || die "the Docker daemon is not reachable -- start Docker Desktop (or dockerd) and run this again"
git -C "$tree" rev-parse --is-inside-work-tree >/dev/null 2>&1 \
  || die "$tree is not a git checkout (the export is git ls-files)"

# The grid comes from the record, so the run cannot drift from the shape the pin
# was measured on; a record of an explicit column list is refused, not guessed.
shape="$(python3 - "$record" <<'PY'
import json
import sys
data = json.load(open(sys.argv[1]))
measurement = data.get("measurement", data)
radius = measurement.get("grid_radius")
if not isinstance(radius, int):
    sys.exit("the record was measured on an explicit column list, not a grid")
seeds = measurement.get("seeds")
if not seeds:
    sys.exit("the record names no seed")
print(",".join(str(seed) for seed in seeds), radius)
PY
)" || die "cannot read the run's shape from $record"
seeds="${shape%% *}"
radius="${shape##* }"

dockerfile_hash() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | cut -c1-12
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | cut -c1-12
  else
    die "no sha256sum or shasum -- needed to tag the toolchain image"
  fi
}

ensure_image() { # tag, then the docker build arguments
  local tag="$1"
  shift
  if docker image inspect "$tag" >/dev/null 2>&1; then
    printf 'toolchain %s\n' "$tag"
    return 0
  fi
  printf 'building toolchain %s (first run for this Dockerfile)\n' "$tag"
  docker build --quiet "$@" -t "$tag" "$here" >/dev/null
}

hash="$(dockerfile_hash "$dockerfile")"
tmpdir="${TMPDIR:-/tmp}"
work="$(mktemp -d "${tmpdir%/}/plusmc-linux-seam.XXXXXX")"
out="$work/light-seam-linux-$arch.json"
cleanup() {
  local exit_status=$?
  trap - EXIT
  if [ "$exit_status" -ne 0 ] && [ -s "$out" ]; then
    printf '\ncheck failed; the measurement is kept at %s\n' "$out" >&2
  else
    rm -rf "$work"
  fi
  exit "$exit_status"
}
trap cleanup EXIT

if [ "$arch" = amd64 ]; then
  image="plusmc-linux-toolchain:deps-$hash"
  ensure_image "$image" --platform "$platform" -f "$dockerfile"

  printf 'building plusmc-light-seam from %s (ubuntu 24.04, gcc, %s)\n' "$tree" "$platform"
  (cd "$tree" && git ls-files -z --cached --others --exclude-standard | tar --null -T - -cf -) \
    | docker run --rm -i --platform "$platform" -v "$work:/out" \
        -e SEEDS="$seeds" -e RADIUS="$radius" -e RECORD_NAME="$(basename "$out")" "$image" bash -euc '
        arch="$(uname -m)"
        test "$arch" = x86_64 || { echo "container arch is $arch, not x86_64" >&2; exit 1; }
        mkdir -p /src
        tar -x -C /src
        cmake -S /src -B /tmp/build -G Ninja \
          -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
          -DCMAKE_CXX_COMPILER=g++ >&2
        cmake --build /tmp/build --target plusmc-light-seam >&2
        /tmp/build/plusmc-light-seam --seeds "$SEEDS" --radius "$RADIUS" \
          --json "/out/$RECORD_NAME" >&2
      '
else
  cross="plusmc-linux-toolchain:arm64-cross-$hash"
  run_image="plusmc-linux-toolchain:arm64-run-$hash"
  ensure_image "$cross" -f "$dockerfile" --target build
  ensure_image "$run_image" --platform "$platform" -f "$dockerfile" --target run

  printf 'cross-compiling plusmc-light-seam for aarch64 from %s\n' "$tree"
  (cd "$tree" && git ls-files -z --cached --others --exclude-standard | tar --null -T - -cf -) \
    | docker run --rm -i -v "$work:/out" "$cross" bash -euc '
        mkdir -p /src
        tar -x -C /src
        cmake -S /src -B /tmp/build -G Ninja \
          -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
          -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
          -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ \
          -DCMAKE_LIBRARY_ARCHITECTURE=aarch64-linux-gnu >&2
        cmake --build /tmp/build --target plusmc-light-seam >&2
        tar -C /tmp/build -cf /out/arm64-seam.tar plusmc-light-seam
      '

  printf 'running the aarch64 probe under %s (the slow leg: ~72 min for 75 columns)\n' "$platform"
  docker run --rm --platform "$platform" -v "$work:/out" \
    -e SEEDS="$seeds" -e RADIUS="$radius" -e RECORD_NAME="$(basename "$out")" "$run_image" bash -euc '
      arch="$(uname -m)"
      test "$arch" = aarch64 || { echo "container arch is $arch, not aarch64" >&2; exit 1; }
      mkdir -p /run
      tar -x -C /run -f /out/arm64-seam.tar
      cd /run
      ./plusmc-light-seam --seeds "$SEEDS" --radius "$RADIUS" --json "/out/$RECORD_NAME" >&2
    '
fi

printf '\ncomparing with %s\n' "$record"
python3 "$root/bench/seam_track.py" --measurement "$out" --against "$record" --fail-on-change
