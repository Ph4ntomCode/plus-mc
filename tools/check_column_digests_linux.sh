#!/usr/bin/env bash
# +MC -- the decorated-column pin under Linux gcc, in a container.
#
# The 75-column record is compared across compilers because a generator value
# can depend on what the stack held: the geode's borrowed amplitude array was
# stable on clang and moved one column under gcc. CI's ubuntu-gcc runner is
# where that shows up first; this is the same check on a machine with Docker,
# without waiting for CI:
#
#   tools/check_column_digests_linux.sh
#
# It exports a checkout (tracked and untracked files, nothing ignored), builds
# `plusmc-column-digest` from it in ubuntu:24.04 with gcc for linux/amd64 --
# emulated on an arm64 host, never the host's own arch -- and compares the
# record with the pin `.github/workflows/ci.yml` names. On a tree that matches
# the pin the two records differ only in their `compiler` line.
#
# `--arch arm64` runs the emulated leg instead: the probe is cross-compiled
# with Ubuntu's aarch64 gcc (`tools/linux-digest-arm64.Dockerfile`) and run in
# an arm64 container -- native on an arm64 host, qemu-emulated on x86_64. That
# is the slow leg (the 22 Oct probe took 5,197 s for 75 columns), and it is
# compared with the same pin.
#
# `--tree DIR` digests another git checkout instead of this one. The control
# is the same tree with the geode fix undone, which this check is expected to
# fail on the one column the fix moved.
#
# The checkout is copied, never mounted or built in: the tree is streamed into
# the container, the build happens there, and the record (or, for aarch64, the
# probe itself) is written to the run's temporary directory, so nothing is
# written to the checkout. The toolchain images are tagged with their
# Dockerfile's hash, so a repeat run reuses one without touching the network
# while an edited Dockerfile builds a new tag instead of silently reusing an old
# image. On failure the record is kept and its path printed, so the moved
# columns can be read without a second build.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
# The pin is whatever CI compares against, read from the workflow rather than
# copied here: a re-recorded baseline is one edit there, and this check cannot
# drift away from the job it stands in for.
baseline="$(sed -n 's/^[[:space:]]*COLUMN_BASELINE:[[:space:]]*//p' \
  "$root/.github/workflows/ci.yml" | head -1 | tr -d '\r')"
platform="linux/amd64"
dockerfile="$here/linux-digest.Dockerfile"
tree="$root"
arch="amd64"

die() {
  printf '%s\n' "$*" >&2
  exit 1
}

usage() {
  printf 'usage: %s [--arch amd64|arm64] [--tree DIR]\n' "${BASH_SOURCE[0]}"
  printf '  ARCH  amd64 (default, native gcc) or arm64 (cross-compiled, emulated)\n'
  printf '  DIR   checkout to digest (default: the tree this script is in)\n'
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

[ -d "$tree" ] || die "no such directory: $tree"
[ -f "$tree/CMakeLists.txt" ] || die "$tree is not the +MC tree (no CMakeLists.txt)"
[ -n "$baseline" ] || die "no COLUMN_BASELINE in $root/.github/workflows/ci.yml"
[ -f "$root/$baseline" ] || die "missing the pin $baseline"
[ -f "$dockerfile" ] || die "missing $dockerfile"
command -v docker >/dev/null 2>&1 || die "docker is not installed -- this check needs Docker"
command -v python3 >/dev/null 2>&1 || die "python3 is not installed -- needed for the comparison"
docker info >/dev/null 2>&1 \
  || die "the Docker daemon is not reachable -- start Docker Desktop (or dockerd) and run this again"
git -C "$tree" rev-parse --is-inside-work-tree >/dev/null 2>&1 \
  || die "$tree is not a git checkout (the export is git ls-files)"

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
work="$(mktemp -d "${tmpdir%/}/plusmc-linux-digest.XXXXXX")"
record="$work/column-digest-linux-$arch.json"
cleanup() {
  local exit_status=$?
  trap - EXIT
  if [ "$exit_status" -ne 0 ] && [ -s "$record" ]; then
    printf '\ncheck failed; the record is kept at %s\n' "$record" >&2
  else
    rm -rf "$work"
  fi
  exit "$exit_status"
}
trap cleanup EXIT

if [ "$arch" = amd64 ]; then
  image="plusmc-linux-toolchain:deps-$hash"
  ensure_image "$image" --platform "$platform" -f "$dockerfile"

  printf 'building plusmc-column-digest from %s (ubuntu 24.04, gcc, %s)\n' "$tree" "$platform"
  (cd "$tree" && git ls-files -z --cached --others --exclude-standard | tar --null -T - -cf -) \
    | docker run --rm -i --platform "$platform" -v "$work:/out" \
        -e RECORD_NAME="$(basename "$record")" "$image" bash -euc '
        arch="$(uname -m)"
        test "$arch" = x86_64 || { echo "container arch is $arch, not x86_64" >&2; exit 1; }
        mkdir -p /src
        tar -x -C /src
        cmake -S /src -B /tmp/build -G Ninja \
          -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
          -DCMAKE_CXX_COMPILER=g++ >&2
        cmake --build /tmp/build --target plusmc-column-digest >&2
        /tmp/build/plusmc-column-digest --json "/out/$RECORD_NAME" >&2
      '
else
  image="plusmc-linux-toolchain:arm64-cross-$hash"
  run_image="plusmc-linux-toolchain:arm64-run-$hash"
  ensure_image "$image" -f "$dockerfile" --target build
  ensure_image "$run_image" --platform "$platform" -f "$dockerfile" --target run

  printf 'cross-compiling plusmc-column-digest for aarch64 from %s\n' "$tree"
  (cd "$tree" && git ls-files -z --cached --others --exclude-standard | tar --null -T - -cf -) \
    | docker run --rm -i -v "$work:/out" "$image" bash -euc '
        mkdir -p /src
        tar -x -C /src
        cmake -S /src -B /tmp/build -G Ninja \
          -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
          -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
          -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ \
          -DCMAKE_LIBRARY_ARCHITECTURE=aarch64-linux-gnu >&2
        cmake --build /tmp/build --target plusmc-column-digest >&2
        tar -C /tmp/build -cf /out/arm64-probe.tar plusmc-column-digest
      '

  printf 'running the aarch64 probe under %s (the slow leg)\n' "$platform"
  docker run --rm --platform "$platform" -v "$work:/out" \
    -e RECORD_NAME="$(basename "$record")" "$run_image" bash -euc '
      arch="$(uname -m)"
      test "$arch" = aarch64 || { echo "container arch is $arch, not aarch64" >&2; exit 1; }
      mkdir -p /run
      tar -x -C /run -f /out/arm64-probe.tar
      /run/plusmc-column-digest --json "/out/$RECORD_NAME" >&2
    '
fi

printf '\ncomparing with %s\n' "$baseline"
( cd "$work" && python3 "$root/tools/compare_column_digests.py" \
    --baseline "$root/$baseline" "$(basename "$record")" )
