#!/usr/bin/env bash
set -euo pipefail

if [[ ${1:-} == -* || ! -f "${1:-}" ]]; then
    printf 'Usage: %s <existing Nix file> [nix-shell options]\n' "${0##*/}" >&2
    exit 2
fi

# nix-shell otherwise looks up its launch Bash through ambient <nixpkgs>.
# A host Bash can also put its directory ahead of the pinned tools on PATH.
if [[ -z "${NIX_BUILD_SHELL:-}" ]]; then
    shell_path="$(nix-build "$1" -A portalShell --no-out-link)"
    if [[ -z "$shell_path" || "$shell_path" == *$'\n'* ]]; then
        printf '%s\n' 'Expected one output path from nix-build -A portalShell.' >&2
        exit 1
    fi
    export NIX_BUILD_SHELL="$shell_path/bin/bash"
fi
exec nix-shell "$@"
