# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Aobus Contributors

# Resolve nix-build for a Darwin portal whose non-interactive PATH omits Nix.
# Candidate arguments are bin directories, ordered from most to least preferred.
aobus_require_nix_build()
{
    if command -v nix-build >/dev/null 2>&1; then
        return 0
    fi

    local darwin_nix_bin
    for darwin_nix_bin in "$@"; do
        if [[ -x "$darwin_nix_bin/nix-build" ]]; then
            export PATH="$darwin_nix_bin:$PATH"
            return 0
        fi
    done

    echo "error: nix-build is unavailable; install Nix before running ./ao" >&2
    return 127
}
