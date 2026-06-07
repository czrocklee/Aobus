#!/usr/bin/env bash
# Populate the real repo ccache using the same path shape C2 sees under bwrap.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
REPO="${AOBUS_AGENT_REPO:-$DEFAULT_REPO}"
REPO="$(cd "$REPO" && pwd -P)"

cd "$REPO"
if [[ -z "${IN_NIX_SHELL:-}" ]]; then
  exec nix-shell --run "$(printf '%q ' "$0" "$@")"
fi

# shellcheck disable=SC1091
. "$SCRIPT_DIR/common.sh"

command -v bwrap >/dev/null 2>&1 || {
  echo "warm-main-ccache: bwrap is required" >&2
  exit 2
}
command -v ccache >/dev/null 2>&1 || {
  echo "warm-main-ccache: ccache is required" >&2
  exit 2
}

view_build="${AOBUS_AGENT_BWRAP_BUILD_VIEW:-/tmp/build/debug}"
cache_parent="$REPO/.cache"
cache_dir="$cache_parent/ccache"
mkdir -p "$cache_dir" "$cache_parent/cmake-deps"

created_build_dir=0
host_build="${AOBUS_WARM_CCACHE_BUILD_DIR:-}"
if [ -z "$host_build" ]; then
  host_build="$(mktemp -d /tmp/aobus-warm-main-ccache.XXXXXX)"
  created_build_dir=1
else
  mkdir -p "$host_build"
  host_build="$(cd "$host_build" && pwd -P)"
fi

cleanup() {
  if [ "$created_build_dir" -eq 1 ] && [ "${AOBUS_WARM_CCACHE_KEEP_BUILD:-0}" != "1" ]; then
    rm -rf "$host_build"
  fi
}
trap cleanup EXIT

if [ "$#" -gt 0 ]; then
  targets=("$@")
else
  targets=(ao_test ao_test_gtk aobus-gtk)
fi

ccache_stats() {
  local label="$1"
  echo "warm-main-ccache: $label"
  CCACHE_DIR="$cache_dir" ccache -s
}

echo "warm-main-ccache: repo=$REPO"
echo "warm-main-ccache: host_build=$host_build"
echo "warm-main-ccache: build_view=$view_build"
echo "warm-main-ccache: ccache_dir=$cache_dir"
echo "warm-main-ccache: targets=${targets[*]}"
ccache_stats "before"

export AGENT_CCACHE_LAUNCHER
AGENT_CCACHE_LAUNCHER="$(command -v ccache)"

agent_bwrap_path_view_run "$REPO" "$REPO" "$host_build" "$view_build" "$cache_parent" "-" "-" \
  bash -c '
    set -euo pipefail
    cd "$AOBUS_AGENT_REPO"
    cmake --preset linux-debug -B "$BUILD_DIR" -S "$AOBUS_AGENT_REPO" \
      -DCCACHE_PROGRAM="$AGENT_CCACHE_LAUNCHER" \
      -DFETCHCONTENT_BASE_DIR="$FETCHCONTENT_BASE_DIR"
    cmake --build "$BUILD_DIR" --parallel --target "$@"
  ' bash "${targets[@]}"

ccache_stats "after"
echo "warm-main-ccache: done"
