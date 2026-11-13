---
id: development.dependency-governance
---
# Dependency version governance

This page explains how to identify the authoritative dependency input, inspect a
resolved dependency, and decide whether a pin belongs in the shared contract.
It also identifies binary-distribution checks that version acceptance does not
replace. Use the [dependency upgrade workflow](dependency-upgrade.md) to change pins.

## Policy goal

Aobus resolves native dependencies through different ecosystems:

- a pinned Nixpkgs package set on Linux;
- shared versioned vcpkg registries on macOS and Windows;
- a signed, exact NuGet closure for Windows App SDK/MSBuild dependencies; and
- managed Python environments for repository tooling.

Those ecosystems need not produce identical transitive graphs or binaries. The
contract instead aligns selected direct dependencies and behavior-affecting
capabilities, while preserving enough native identity to reproduce and audit
each platform's resolution.

Linux Nix is the normal lead resolver for routine updates, not the policy source
of truth. A resolved version becomes project policy only through an explicit
`dependency-contract.json` change and the affected native validation.

## Find the source of truth

Use the owner for the question being asked:

| Question | Owner |
|---|---|
| Accepted cross-platform C++/Windows SDK versions, capabilities, and temporary exceptions | `dependency-contract.json` |
| Linux package-set revision and source hash | `nixpkgs.json` |
| vcpkg registry snapshots | `vcpkg-configuration.json` |
| vcpkg ports, features, and exact overrides | `vcpkg.json` |
| macOS compiler, deployment target, and vcpkg tool archive | `script/ao/macos-toolchain.json` |
| Windows tidy LLVM SDK version, archive SHA-256, and required files | `script/ao/windows-llvm-sdk.json` |
| CI Bazel remote-cache proxy version and artifact SHA-256, and its Windows WSL host rootfs SHA-256 | `script/ao/bazel-remote.json` |
| Bazel version | `script/ao/bazel-toolchain.json`, `.bazelversion` |
| Buildifier formatter/linter version and native artifact SHA-256 | `script/ao/buildifier.json` |
| WinUI NuGet closure and trusted source | `app/windows-winui/packages.config`, `app/windows-winui/NuGet.Config` |
| Python, Ruff, mypy, and documentation-parser policy | `script/ao/toolchain.json` |
| Accepted Python tooling and documentation-parser artifacts | `script/ao/docs-requirements.txt`, `script/ao/windows-requirements.txt`, `script/ao/macos-requirements.txt` |
| Installed native and test SDK manifests and hash inputs | verified by `./ao deps report` or `./ao deps verify` |

Do not copy a current version from this guide. Read the owning file or a verified
build report.
The [Bazel build system](bazel.md) projects governed providers into
disposable imports and adds no independent version authority.
Its repository-time bootstrap enters the pinned Nix shell on Linux, allowing
Nix to realize missing pinned outputs. Native macOS and Windows discovery
instead validates ready, explicitly selected SDKs; repository fetch does not
install their producers, ports, Python or SDKs. Its `config/bazel/dependencies.json` holds only
structural import contracts — static header and archive interface fallbacks,
exact library spellings and closure order, metadata-token preconditions, ABI
defines, and native repair entries — with no per-package accepted-version
entries. A version is accepted here, in the shared contract and resolver inputs; at
preparation time the observed provider is matched against its installed
headers, pkg-config metadata and tools, so an upgrade this policy accepts
passes the unchanged structural checks without a Bazel-side version edit.
Centralization alone is not a reason to promote more dependencies into the
shared contract. See [imported
interfaces](bazel.md#imported-interfaces-and-compilation) for the structural
checks applied to observed provider identity.

## Locating an existing dependency

Start with the checkout, target platform, configuration, and the build tree that
actually consumed the dependency. A native VM may own the provider even when
the source is mounted from another host.

1. Inspect the selected tree's provisioned SDK, the dependency report, and
   `build.log`. `./ao deps report` verifies and reports the actual installed
   native and test SDK manifests and their hash inputs; it does not install
   anything or search the host.
2. Identify the resolver in the table above and inspect the relevant resolver
   input or Bazel import declaration for the package or imported interface.
3. Ask that resolver for paths in the target environment. Linux inspection runs
   inside the pinned Nix shell. macOS and Windows C++ libraries normally come
   from the selected vcpkg installation and triplet. WinUI uses the governed
   NuGet closure; SDK and compiler paths come from the native toolchain.
4. Restrict searches to the resulting include, library, source, or installation
   directories. Do not recursively search `/`, unrelated home directories, or
   mounted storage as a fallback.
5. If the provider is absent, report the target, resolver, build tree, and paths
   checked, then follow the owning platform setup. Do not substitute an ambient
   package or change a pin merely to make discovery succeed.

## What belongs in the shared contract

The current governed dependency names and policies are enumerated by
`dependency-contract.json`; do not maintain a second list here. Promote another
dependency only when an explicit contract is cheaper and safer than recurring
diagnosis, for example when:

- observed API or behavior drift caused cross-platform failures;
- templates, macros, public types, or build options create ABI/ODR risk;
- the dependency is security-sensitive; or
- persisted or user-visible semantics depend on one implementation or data set.

A matching upstream version is not proof of build, ABI, or behavioral identity.
The Bazel structural import checks verify declared imported interfaces and
capabilities against the observed provider. Native reports
retain resolver evidence such as Nix output identities or vcpkg registry,
triplet, features, tool version, and `version#port-version`.

## Native resolution rules

### Linux

`shell.nix` imports the exact revision and hash in `nixpkgs.json` and rejects
ambient `NIX_PATH`, channel, or `PATH` packages as substitutes. Project
derivations remain governed by their own source hashes and the shared contract.
Nix evaluation also checks applicable Python, Ruff, mypy, and Buildifier
pins. One package set supplies both the native dependencies and these tools,
so the native pin and the exact tool contracts are coupled: a package-set
change that moves a tool version must update the corresponding contract in
the same change, and a tool upgrade needs a package set that supplies it.

### macOS and Windows vcpkg

`vcpkg-configuration.json` owns registry identity; `vcpkg.json` owns requested
ports, features, and any exact override. macOS bootstraps the verified vcpkg
archive in `script/ao/macos-toolchain.json`; Windows uses Visual Studio's
bundled vcpkg tool. Project triplets own platform linkage and deployment flags.

A package family that must remain coherent, such as split Boost ports, uses a
package-scoped registry rather than one top-level port override. An override
ignores other version constraints and therefore needs a reason and removal
condition in the upgrade review.

The default registry keeps its accepted `baseline` as the resolution floor
and may pin an immutable `reference` commit that supplies the official
version database. Exact manifest overrides then select specific official
recipes from that database — for example CLI11, Catch2, or the Windows host
`python3` build tool — without moving the baseline floor for every other
port. The host `python3` port is dependency-build tooling; it is not the
portal Python, which comes from the governed NuGet closure at the exact
`script/ao/toolchain.json` version.

Repository overlay ports are exceptional and temporary.

#### Temporary overlay ports

The `cmake/vcpkg-ports/stb` overlay is an active, short-lived exception to the
normal resolver order. The official vcpkg `stb` port at the approved baseline
installs `stb_image_resize2` v2.10, whose vertical edge-region calculation
triggers UndefinedBehaviorSanitizer. The overlay shadows the complete `stb`
port: its payload freezes every other header at that baseline's upstream source
revision while replacing only `stb_image_resize2.h` with the
source-hash-verified v2.18 revision already pinned by `shell.nix`. Future
changes to the official port recipe or its other headers are not inherited
while the overlay remains active.

When the overlay was adopted, the first five mechanisms in the
[dependency upgrade resolver order](dependency-upgrade.md#3-resolve-it-on-macos-and-windows)
were rejected for explicit reasons:

1. The approved default baseline contained only the affected v2.10 header.
2. A direct `version>=` constraint could not select the fixed revision because
   it was absent from the official `stb` versions database.
3. An exact manifest override had the same limitation.
4. A package-scoped official registry had no newer maintained `stb` port to
   select.
5. No maintained versioned custom registry supplied this header, and creating a
   permanent project registry for one temporary header replacement would add a
   second dependency authority without an independent contract.

`stb_image_resize2.h` exposes no version macro. Remove the overlay when the
selected official vcpkg port installs a header whose leading version banner is
v2.18 or newer and whose source contains the upstream edge-region fix. Removal
also means deleting the overlay settings in the vcpkg configuration and the
pin-consistency tests, then
passing the focused cover-art loader under macOS ASan/UBSan, the Windows native
TUI tests, and both platforms' full checks with the official port. Do not let
the overlay become an untracked fork or intentionally change unrelated stb
headers in its frozen payload.

### Windows App SDK and C++/WinRT

WinUI's top-level package versions are governed by the shared contract. The
checked-in NuGet files own the exact transitive closure, source mapping, and
required repository signatures. Dependency verification inspects restored
archive identity.

The Windows App Runtime is host state, not the NuGet development closure. Its
identity, installer URL, and SHA-256 are governed separately, and setup also
checks Microsoft Authenticode. The runtime `version` pins that installer and is
the minimum accepted host package version, not an exact installed-version
requirement: Microsoft services framework packages within the same family.
Host detection selects the highest healthy, current-user-registered version
in the governed Microsoft package family and architecture. See
[WinUI setup](windows.md#build-and-run-winui) for the native commands.

### Repository tooling

`script/ao/toolchain.json` is policy. Linux and Windows match its exact Python,
Ruff, and mypy versions. macOS matches Ruff and mypy exactly but accepts the
contracted Homebrew Python major/minor rather than an exact patch.
Linux uses the pinned Nix environment. Windows uses Visual Studio's vcpkg to
fetch the NuGet tool, then NuGet's `python` package for portal CPython; the
manifest's `python3` host port is separate dependency-build tooling. macOS uses
Homebrew base Python and selects a managed tool venv on demand. Native Ruff/mypy
come from the hashed requirements locks, not ambient tools. See the
[Windows](windows.md#prerequisites) and [macOS](macos.md#host-setup) guides for
setup and local-state requirements.

Buildifier is governed separately by `script/ao/buildifier.json`: an exact
version and hash-pinned official macOS/Windows artifacts. Linux selects the
matching package from pinned Nixpkgs through `AO_NIX_BUILDIFIER`, not `PATH`.
Native hosts cache it under their Aobus state root's `tools/buildifier/` and
validate both SHA-256 and version on every use; an existing damaged entry fails
rather than falling back or silently repairing it. `./ao setup buildifier`
(`ao.bat setup buildifier`) explicitly prepares only that tool. Format and lint
also prepare a missing native tool when their scope contains Bazel files; no
C++ SDK is required for a Bazel-only scope. See [Starlark hygiene](linting.md#bazelstarlark-hygiene).

The native requirements files are artifact locks with hashes, not alternative
policy files. Ruff's target version and mypy's `python_version` in
`pyproject.toml` describe supported language syntax; they need not equal the
managed interpreter's patch release.

A `py3-none-any` wheel is still a hash-locked wheel accepted by pip's
`--only-binary` selection, not a source distribution or a compiled-native
claim. Selecting one explicitly — for example when a platform lacks a
compiled CPython wheel for the contracted mypy release — keeps the lock
hash-verified but does not establish native-compiled performance; verify the
selected wheel at runtime on that platform.

The documentation parser follows the same split: `script/ao/toolchain.json`
pins the exact markdown-it-py and mdurl versions, and
`script/ao/docs-requirements.txt` is their shared hash-verified artifact lock.
The native requirement locks include that file with a pip `-r` line, so the
native bootstrap installs the same accepted parser wheels, and the CI
documentation job installs the shared lock directly before
`python -m ao docs check`. The managed-environment fingerprint covers the
included lock, so a changed or missing parser pin rebuilds the native
environment instead of reusing a stale one.

## Enforcement

The normal enforcement path is:

1. the native bootstrap validates its applicable tool contract;
2. the Bazel repository rules admit the installed providers against
   `dependency-contract.json`, rejecting expired exceptions and checking
   versions, interfaces, and capabilities;
3. `./ao deps verify` checks the installed native and test SDK manifests and
   hash inputs against current source inputs; and
4. `./ao check` gates configured dependency admission as part of completion.

Reports are build artifacts, not source files. Preserve before/after reports as
CI or review evidence for dependency changes; summarize governed changes rather
than pasting the complete transitive graph into a pull request.

## Temporary exceptions and security response

Silent skew is not allowed. A temporary platform exception is a narrow entry in
`dependency-contract.json` naming one dependency and platform, allowed version,
technical reason and risk, owner, issue, creation and expiry dates, and exit
condition. Expiry is a UTC calendar date and fails closed. A normal ecosystem
availability exception should not exceed 30 days; security-driven skew should
normally reconcile within 14 days unless security policy is stricter.

Security response need not wait for the routine Nix lead order. Land the fix on
the first viable platform, then update the contract or add a bounded exception
for a lagging platform with compensating controls. Emergency work still uses
immutable sources and hashes and runs the minimum clean build and smoke tests.

Atomicity means the default branch remains in one verifiable state. A normal
governed update changes policy and every affected resolver path together; a
security-first update may use the bounded exception mechanism. It does not mean
unrelated tool updates or formatting changes belong in the same change.

## Binary distribution

A development executable or application bundle is not automatically a complete
release package. Check the actual incorporated libraries, copied runtime files
and assets, not every dependency used by build tools. Include the project's
[MIT notice](../../LICENSE), applicable third-party notices and the
[brand notice](../../asset/brand/LICENSE.txt) when its assets are bundled; verify
redistribution terms for the selected SDK/runtime payloads.

The native macOS triplet links mpg123 statically; the reviewed Windows SDK
links it dynamically. The existing
[LGPL distribution boundary](windows.md#distribution-and-storage-boundaries)
also applies to macOS binaries: choose and validate a compliance route before
publishing. A successful build or public application source alone does not
establish usable library modification/relinking materials.

FDK-AAC has separate obligations. Its [NOTICE](https://gitlab.freedesktop.org/wtaymans/fdk-aac-stripped/-/raw/9896ddc0d08bb3f764f01d5e372bec1c068ad8f5/NOTICE)
requires the complete license text with binary distributions and free
availability of the complete codec source and modifications to recipients;
it grants no patent license. The manifest does not request the vcpkg port's
`he-aac` feature; the current registry's default uses a stripped fork, whereas
Linux selects upstream FDK-AAC through `shell.nix`. Verify the actual installed
source, patches and features rather than inferring either universal HE-AAC
support or patent clearance. Preserve those inputs for the selected release and
qualify unresolved licensing or patent questions before distribution.

## Non-goals

This policy does not require identical transitive graphs, compare vcpkg port
revisions with Nix package metadata, promise cross-OS ABI or bit-for-bit binary
identity, govern every dependency, or require a particular CI provider.
