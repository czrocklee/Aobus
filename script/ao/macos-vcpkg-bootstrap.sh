#!/usr/bin/env bash
# Host bootstrap used by the macOS branch of ./ao. Keep this compatible with
# Apple's Bash 3.2: it is sourced before the Homebrew toolchain is available.

aobus_macos_error() {
    printf 'Aobus macOS bootstrap: %s\n' "$*" >&2
}

aobus_macos_normalize_locale() {
    # Darwin has no C.UTF-8 locale: it silently resolves to the ASCII C locale.
    # Preserve UTF-8 behavior when automation forwards that Linux locale name.
    if [[ "${LC_ALL:-}" == "C.UTF-8" ]]; then
        export LC_ALL="en_US.UTF-8"
    fi
    if [[ "${LC_CTYPE:-}" == "C.UTF-8" ]]; then
        export LC_CTYPE="en_US.UTF-8"
    fi
    if [[ "${LANG:-}" == "C.UTF-8" || -z "${LANG:-}" ]]; then
        export LANG="en_US.UTF-8"
    fi
}

aobus_macos_default_state_root() {
    local home_directory="${1:-${HOME:-}}"
    if [[ -z "$home_directory" ]]; then
        aobus_macos_error 'HOME is unset; set AOBUS_STATE_ROOT to a local directory.'
        return 1
    fi
    printf '%s\n' "$home_directory/Library/Caches/Aobus"
}

aobus_macos_prepare_ccache_environment() {
    local project_root="$1"
    local state_root="$2"

    export CCACHE_DIR="$state_root/ccache"
    export CCACHE_BASEDIR="$project_root"
    export CCACHE_MAXSIZE="10G"
    export CCACHE_COMPRESS=1
    export CCACHE_SLOPPINESS="time_macros"
    mkdir -p "$CCACHE_DIR"
}

aobus_macos_triplet() {
    case "${1:-}" in
        x86_64)
            printf '%s\n' 'x64-aobus-osx'
            ;;
        arm64)
            printf '%s\n' 'arm64-aobus-osx'
            ;;
        *)
            aobus_macos_error "unsupported architecture '${1:-unknown}'; expected x86_64 or arm64."
            return 1
            ;;
    esac
}

aobus_macos_find_brew() {
    if command -v brew >/dev/null 2>&1; then
        command -v brew
        return 0
    fi
    local candidate
    for candidate in /usr/local/bin/brew /opt/homebrew/bin/brew; do
        if [[ -x "$candidate" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    aobus_macos_error 'Homebrew is unavailable; install it from https://brew.sh/ first.'
    return 1
}

aobus_macos_formula_prefix() {
    local brew_executable="$1"
    local formula="$2"
    local prefix
    if ! prefix="$("$brew_executable" --prefix "$formula" 2>/dev/null)" || [[ ! -d "$prefix" ]]; then
        aobus_macos_error "Homebrew formula '$formula' is unavailable; run: brew install $formula"
        return 1
    fi
    printf '%s\n' "$prefix"
}

aobus_macos_prepare_python() {
    local project_root="$1"
    local config_file="$project_root/script/ao/macos-toolchain.json"
    local brew_executable python_formula python_version python_prefix python_executable

    aobus_macos_normalize_locale
    if [[ ! -f "$config_file" ]]; then
        aobus_macos_error "toolchain lock is missing: $config_file"
        return 1
    fi
    if ! brew_executable="$(aobus_macos_find_brew)"; then
        return 1
    fi

    # The system Python is too old for the portal package. Read only the two
    # bootstrap fields with it; the selected Homebrew Python reads the complete
    # lock before any build tools are used.
    if ! python_formula="$(/usr/bin/python3 -c \
        'import json,sys; print(json.load(open(sys.argv[1]))["homebrew"]["pythonFormula"])' \
        "$config_file")"; then
        aobus_macos_error "cannot read $config_file with /usr/bin/python3."
        return 1
    fi
    if ! python_version="$(/usr/bin/python3 -c \
        'import json,sys; print(json.load(open(sys.argv[1]))["homebrew"]["pythonMajorMinorVersion"])' \
        "$config_file")"; then
        aobus_macos_error "cannot read $config_file with /usr/bin/python3."
        return 1
    fi
    if ! python_prefix="$(aobus_macos_formula_prefix "$brew_executable" "$python_formula")"; then
        return 1
    fi
    python_executable="$python_prefix/bin/python$python_version"
    if [[ ! -x "$python_executable" ]]; then
        aobus_macos_error "'$python_formula' does not provide $python_executable."
        return 1
    fi
    if ! "$python_executable" -I -c \
        'import sys; raise SystemExit(sys.version_info[:2] != tuple(map(int, sys.argv[1].split("."))))' \
        "$python_version"; then
        aobus_macos_error "$python_executable is not Python $python_version."
        return 1
    fi

    export AOBUS_HOMEBREW="$brew_executable"
    export AOBUS_PYTHON="$python_executable"
    export PATH="$python_prefix/bin:$PATH"
    if [[ -z "${AOBUS_STATE_ROOT:-}" ]]; then
        if ! AOBUS_STATE_ROOT="$(aobus_macos_default_state_root)"; then
            return 1
        fi
        export AOBUS_STATE_ROOT
    fi
}

aobus_macos_prepare_python_tools() {
    local project_root="$1"
    local result_file managed_python
    result_file="$(mktemp "${TMPDIR:-/tmp}/aobus-python-env.XXXXXX")"
    if ! "$AOBUS_PYTHON" -m ao.core.pythonenv \
        --project-root "$project_root" \
        --state-root "$AOBUS_STATE_ROOT" \
        --result-file "$result_file" \
        --platform macos; then
        rm -f "$result_file"
        return 1
    fi
    managed_python=""
    IFS= read -r managed_python < "$result_file" || true
    rm -f "$result_file"
    if [[ ! -x "$managed_python" ]]; then
        aobus_macos_error "managed Python environment is incomplete: $managed_python"
        return 1
    fi
    export AOBUS_PYTHON="$managed_python"
}

aobus_macos_load_toolchain_lock() {
    local project_root="$1"
    local config_file="$project_root/script/ao/macos-toolchain.json"
    local values
    if ! values="$("$AOBUS_PYTHON" -I -c '
import json
import sys

config = json.load(open(sys.argv[1], encoding="utf-8"))
schema = config.get("schemaVersion")
if schema != 1:
    raise SystemExit("unsupported macOS toolchain schema: " + repr(schema))
homebrew = config["homebrew"]
vcpkg = config["vcpkg"]
print("\t".join((
    config["deploymentTarget"],
    homebrew["llvmFormula"],
    homebrew["llvmMajorVersion"],
    vcpkg["revision"],
    vcpkg["archiveUrl"],
    vcpkg["archiveSha256"],
)))
' "$config_file")"; then
        aobus_macos_error "cannot read a valid toolchain lock from $config_file."
        return 1
    fi
    IFS=$'\t' read -r \
        AOBUS_MACOS_DEPLOYMENT_TARGET \
        AOBUS_MACOS_LLVM_FORMULA \
        AOBUS_MACOS_LLVM_MAJOR \
        AOBUS_MACOS_VCPKG_REVISION \
        AOBUS_MACOS_VCPKG_ARCHIVE_URL \
        AOBUS_MACOS_VCPKG_ARCHIVE_SHA256 <<< "$values"
    if [[ -z "$AOBUS_MACOS_DEPLOYMENT_TARGET" || -z "$AOBUS_MACOS_VCPKG_ARCHIVE_SHA256" ]]; then
        aobus_macos_error "toolchain lock contains an empty required value: $config_file"
        return 1
    fi
    export AOBUS_MACOS_DEPLOYMENT_TARGET AOBUS_MACOS_LLVM_FORMULA AOBUS_MACOS_LLVM_MAJOR
    export AOBUS_MACOS_VCPKG_REVISION AOBUS_MACOS_VCPKG_ARCHIVE_URL AOBUS_MACOS_VCPKG_ARCHIVE_SHA256
}

aobus_macos_sha256() {
    shasum -a 256 "$1" | awk '{print $1}'
}

aobus_macos_install_pinned_vcpkg() (
    local destination="$1"
    local archive_url="$2"
    local archive_sha256="$3"
    local state_root="$4"
    local revision="$5"
    local download_directory="$state_root/cache/vcpkg/downloads"
    local archive="$download_directory/vcpkg-$revision.tar.gz"
    local tools_directory lock_directory owner_pid attempts staging downloaded actual_sha256

    tools_directory="$(dirname "$destination")"
    lock_directory="$tools_directory/.vcpkg-$revision.lock"
    mkdir -p "$tools_directory" "$download_directory"

    attempts=0
    while ! mkdir "$lock_directory" 2>/dev/null; do
        if [[ -x "$destination/vcpkg" && -f "$destination/scripts/buildsystems/vcpkg.cmake" ]]; then
            return 0
        fi
        owner_pid=""
        if [[ -f "$lock_directory/pid" ]]; then
            IFS= read -r owner_pid < "$lock_directory/pid" || true
        fi
        if [[ "$owner_pid" =~ ^[0-9]+$ ]] && ! kill -0 "$owner_pid" 2>/dev/null; then
            rm -f "$lock_directory/pid"
            rmdir "$lock_directory" 2>/dev/null || true
            continue
        fi
        attempts=$((attempts + 1))
        if [[ "$attempts" -ge 600 ]]; then
            aobus_macos_error "timed out waiting for the vcpkg bootstrap lock: $lock_directory"
            return 1
        fi
        sleep 1
    done
    printf '%s\n' "$$" > "$lock_directory/pid"
    staging=""
    downloaded=""
    trap 'if [[ -n "$staging" && -d "$staging" ]]; then rm -rf "$staging"; fi; if [[ -n "$downloaded" ]]; then rm -f "$downloaded"; fi; rm -f "$lock_directory/pid"; rmdir "$lock_directory" 2>/dev/null || true' EXIT

    if [[ -x "$destination/vcpkg" && -f "$destination/scripts/buildsystems/vcpkg.cmake" ]]; then
        return 0
    fi
    if [[ -e "$destination" ]]; then
        aobus_macos_error "managed vcpkg root is incomplete: $destination"
        aobus_macos_error 'move that directory aside, then run ./ao again.'
        return 1
    fi

    actual_sha256=""
    if [[ -f "$archive" ]]; then
        actual_sha256="$(aobus_macos_sha256 "$archive")"
    fi
    if [[ "$actual_sha256" != "$archive_sha256" ]]; then
        downloaded="$(mktemp "$download_directory/.vcpkg-$revision.XXXXXX")"
        printf 'Downloading pinned vcpkg %s...\n' "$revision"
        if ! curl --fail --location --retry 3 --output "$downloaded" "$archive_url"; then
            aobus_macos_error "failed to download $archive_url"
            return 1
        fi
        actual_sha256="$(aobus_macos_sha256 "$downloaded")"
        if [[ "$actual_sha256" != "$archive_sha256" ]]; then
            aobus_macos_error "vcpkg archive SHA-256 mismatch: expected $archive_sha256, got $actual_sha256"
            aobus_macos_error 'follow the verified archive-recovery procedure in doc/development/dependency-upgrade.md.'
            return 1
        fi
        mv -f "$downloaded" "$archive"
        downloaded=""
    fi

    staging="$(mktemp -d "$tools_directory/.vcpkg-$revision.XXXXXX")"
    if ! tar -xzf "$archive" -C "$staging" --strip-components=1; then
        aobus_macos_error "failed to extract the pinned vcpkg archive: $archive"
        return 1
    fi
    if ! (cd "$staging" && ./bootstrap-vcpkg.sh -disableMetrics); then
        aobus_macos_error 'vcpkg bootstrap failed.'
        return 1
    fi
    if [[ ! -x "$staging/vcpkg" || ! -f "$staging/scripts/buildsystems/vcpkg.cmake" ]]; then
        aobus_macos_error 'vcpkg bootstrap completed without a usable toolchain.'
        return 1
    fi
    mv "$staging" "$destination"
    staging=""
)

aobus_macos_prepare_expected_shim() {
    local llvm_root="$1"
    local llvm_version="$2"
    local state_root="$3"
    local source_header="$llvm_root/include/c++/v1/__expected/expected.h"
    local shim_root="$state_root/tools/libcxx-expected-shim/$llvm_version"
    local destination_directory="$shim_root/__expected"
    local destination_header="$destination_directory/expected.h"
    local temporary_header source_count output_count

    if [[ ! -f "$source_header" ]]; then
        aobus_macos_error "LLVM libc++ expected header is missing: $source_header"
        return 1
    fi
    source_count="$(grep -c '^#  if _LIBCPP_STD_VER >= 26$' "$source_header" || true)"
    if [[ "$source_count" != "5" ]]; then
        aobus_macos_error "LLVM libc++ expected header has $source_count C++26 gates; expected exactly 5."
        aobus_macos_error 'Re-test whether the Aobus expected shim is still required before updating the transform.'
        return 1
    fi
    mkdir -p "$destination_directory"
    temporary_header="$(mktemp "$destination_directory/.expected.XXXXXX")"
    if ! sed 's/^#  if _LIBCPP_STD_VER >= 26$/#  if 0/' "$source_header" > "$temporary_header"; then
        rm -f "$temporary_header"
        aobus_macos_error 'failed to generate the libc++ expected shim.'
        return 1
    fi
    output_count="$(grep -c '^#  if _LIBCPP_STD_VER >= 26$' "$temporary_header" || true)"
    if [[ "$output_count" != "0" ]]; then
        rm -f "$temporary_header"
        aobus_macos_error 'generated libc++ expected shim still contains active C++26 gates.'
        return 1
    fi
    if [[ -f "$destination_header" ]] && cmp -s "$temporary_header" "$destination_header"; then
        rm -f "$temporary_header"
    else
        mv -f "$temporary_header" "$destination_header"
    fi
    export AOBUS_LIBCXX_EXPECTED_SHIM="$shim_root"
}

aobus_macos_prepare_build_environment() {
    local project_root="$1"
    local state_root llvm_root llvm_version cmake_root ninja_root pkgconf_root architecture
    local autoconf_root autoconf_archive_root automake_root libtool_root
    local vcpkg_root

    if ! aobus_macos_load_toolchain_lock "$project_root"; then
        return 1
    fi
    if [[ -n "${AOBUS_STATE_ROOT:-}" ]]; then
        state_root="$AOBUS_STATE_ROOT"
    elif ! state_root="$(aobus_macos_default_state_root)"; then
        return 1
    fi
    export AOBUS_STATE_ROOT="$state_root"
    aobus_macos_prepare_ccache_environment "$project_root" "$state_root"

    if ! llvm_root="$(aobus_macos_formula_prefix "$AOBUS_HOMEBREW" "$AOBUS_MACOS_LLVM_FORMULA")"; then
        return 1
    fi
    if ! cmake_root="$(aobus_macos_formula_prefix "$AOBUS_HOMEBREW" cmake)"; then
        return 1
    fi
    if ! ninja_root="$(aobus_macos_formula_prefix "$AOBUS_HOMEBREW" ninja)"; then
        return 1
    fi
    if ! pkgconf_root="$(aobus_macos_formula_prefix "$AOBUS_HOMEBREW" pkgconf)"; then
        return 1
    fi
    if ! autoconf_root="$(aobus_macos_formula_prefix "$AOBUS_HOMEBREW" autoconf)"; then
        return 1
    fi
    if ! autoconf_archive_root="$(aobus_macos_formula_prefix "$AOBUS_HOMEBREW" autoconf-archive)"; then
        return 1
    fi
    if ! automake_root="$(aobus_macos_formula_prefix "$AOBUS_HOMEBREW" automake)"; then
        return 1
    fi
    if ! libtool_root="$(aobus_macos_formula_prefix "$AOBUS_HOMEBREW" libtool)"; then
        return 1
    fi
    llvm_version="$("$llvm_root/bin/clang++" -dumpversion)"
    if [[ "${llvm_version%%.*}" != "$AOBUS_MACOS_LLVM_MAJOR" ]]; then
        aobus_macos_error "'$AOBUS_MACOS_LLVM_FORMULA' provides Clang $llvm_version; expected major $AOBUS_MACOS_LLVM_MAJOR."
        return 1
    fi

    architecture="$(uname -m)"
    if ! AOBUS_VCPKG_TRIPLET="$(aobus_macos_triplet "$architecture")"; then
        return 1
    fi
    export AOBUS_VCPKG_TRIPLET
    export AOBUS_LLVM_ROOT="$llvm_root"
    export CC="$llvm_root/bin/clang"
    export CXX="$llvm_root/bin/clang++"
    export MACOSX_DEPLOYMENT_TARGET="$AOBUS_MACOS_DEPLOYMENT_TARGET"
    export PATH="$llvm_root/bin:$cmake_root/bin:$ninja_root/bin:$pkgconf_root/bin:$autoconf_root/bin:$automake_root/bin:$libtool_root/libexec/gnubin:$PATH"
    # Resolving the archive prefix above is intentional even though it has no
    # executable: autoreconf discovers its macro collection through Homebrew.
    : "$autoconf_archive_root"

    if [[ -n "${VCPKG_ROOT:-}" ]]; then
        vcpkg_root="$VCPKG_ROOT"
    else
        vcpkg_root="$state_root/tools/vcpkg/$AOBUS_MACOS_VCPKG_REVISION"
        if ! aobus_macos_install_pinned_vcpkg \
            "$vcpkg_root" \
            "$AOBUS_MACOS_VCPKG_ARCHIVE_URL" \
            "$AOBUS_MACOS_VCPKG_ARCHIVE_SHA256" \
            "$state_root" \
            "$AOBUS_MACOS_VCPKG_REVISION"; then
            return 1
        fi
    fi
    if [[ ! -x "$vcpkg_root/vcpkg" || ! -f "$vcpkg_root/scripts/buildsystems/vcpkg.cmake" ]]; then
        aobus_macos_error "VCPKG_ROOT does not contain a bootstrapped vcpkg checkout: $vcpkg_root"
        return 1
    fi
    export VCPKG_ROOT="$vcpkg_root"
    export VCPKG_DISABLE_METRICS=1
    export VCPKG_DOWNLOADS="$state_root/cache/vcpkg/downloads"
    export VCPKG_DEFAULT_BINARY_CACHE="$state_root/cache/vcpkg/binaries"
    mkdir -p "$VCPKG_DOWNLOADS" "$VCPKG_DEFAULT_BINARY_CACHE"

    if ! aobus_macos_prepare_expected_shim "$llvm_root" "$llvm_version" "$state_root"; then
        return 1
    fi
}
