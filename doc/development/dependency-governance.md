---
id: development.dependency-governance
---
# Dependency version governance

This page explains how to identify the authoritative dependency input, inspect a
resolved dependency, and decide whether a pin belongs in the shared contract.
Use the [dependency upgrade workflow](dependency-upgrade.md) to change pins.

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
| Accepted cross-platform C++/Windows SDK versions, required targets, capabilities, and temporary exceptions | `dependency-contract.json` |
| Linux package-set revision and source hash | `nixpkgs.json` |
| vcpkg registry snapshots | `vcpkg-configuration.json` |
| vcpkg ports, features, and exact overrides | `vcpkg.json` |
| macOS compiler, deployment target, and vcpkg tool archive | `script/ao/macos-toolchain.json` |
| WinUI NuGet closure and trusted source | `app/windows-winui/packages.config`, `app/windows-winui/NuGet.Config` |
| Python, Ruff, and mypy policy | `script/ao/toolchain.json` |
| Accepted native Python artifacts | `script/ao/windows-requirements.txt`, `script/ao/macos-requirements.txt` |
| Configured native resolution | `<build>/aobus-dependencies.json`, verified by `./ao deps report` or `./ao deps verify` |

Do not copy a current version from this guide. Read the owning file or a verified
build report.

## Locating an existing dependency

Start with the checkout, target platform, configuration, and the build tree that
actually consumed the dependency. A native VM may own the provider even when
the source is mounted from another host.

1. Inspect the build's `CMakeCache.txt`, `aobus-dependencies.json`, and
   `build.log`. `./ao deps report` verifies and reports an already configured
   tree; it does not configure one or search the host.
2. Identify the resolver in the table above and inspect the relevant
   `CMakeLists.txt` or `cmake/` module for the package or imported target.
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
CMake also checks declared imported targets and capabilities. Native reports
retain resolver evidence such as Nix output identities or vcpkg registry,
triplet, features, tool version, and `version#port-version`.

Conditionally disabled dependencies are `not-applicable`; they are not reported
as verified or missing.

## Native resolution rules

### Linux

`shell.nix` imports the exact revision and hash in `nixpkgs.json` and rejects
ambient `NIX_PATH`, channel, or `PATH` packages as substitutes. Project
derivations remain governed by their own source hashes and the shared contract.
Nix evaluation also checks applicable Python, Ruff, mypy, and compiler-cache
pins.

### macOS and Windows vcpkg

`vcpkg-configuration.json` owns registry identity; `vcpkg.json` owns requested
ports, features, and any exact override. macOS bootstraps the verified vcpkg
archive in `script/ao/macos-toolchain.json`; Windows uses Visual Studio's
bundled vcpkg tool. Project triplets own platform linkage and deployment flags.

A package family that must remain coherent, such as split Boost ports, uses a
package-scoped registry rather than one top-level port override. An override
ignores other version constraints and therefore needs a reason and removal
condition in the upgrade review.

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
also means deleting the preset overlay settings and pin-consistency tests, then
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

The native requirements files are artifact locks with hashes, not alternative
policy files. Ruff's target version and mypy's `python_version` in
`pyproject.toml` describe supported language syntax; they need not equal the
managed interpreter's patch release.

## Enforcement

The normal enforcement path is:

1. the native bootstrap validates its applicable tool contract;
2. CMake reads `dependency-contract.json`, rejects expired exceptions, discovers
   active packages, and checks versions, targets, and capabilities;
3. CMake writes `aobus-dependencies.json` with contract and resolver identity;
4. `./ao deps verify` checks that report against current source inputs; and
5. `./ao check` verifies dependencies after building and before test suites.

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

## Non-goals

This policy does not require identical transitive graphs, compare vcpkg port
revisions with Nix package metadata, promise cross-OS ABI or bit-for-bit binary
identity, govern every dependency, or require a particular CI provider.
