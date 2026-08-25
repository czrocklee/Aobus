---
id: development.dependency-upgrade
type: development
status: current
domain: development
summary: Defines the workflow for upgrading governed dependencies and development tools.
---
# Dependency upgrade workflow

This document is the contributor procedure for changing Aobus dependency and
development-tool pins. The governing policy is described in
[dependency governance](dependency-governance.md).

Run every command from the repository root. Linux and macOS commands use
`./ao`; native Windows commands use `ao.bat`. Read
[macOS development](macos.md) or [Windows development](windows.md) before using
the corresponding native host.

## Before starting

Classify the change:

1. Nixpkgs batch update;
2. one governed C++ dependency;
3. a monitor-only dependency;
4. Python/Ruff/mypy tooling;
5. emergency security update.

Do not combine unrelated classes merely because they all contain version
numbers. In particular, a Ruff update and a Boost update should normally be
separate pull requests.

Capture the current state before editing:

```bash
./ao build
./ao deps report --json /tmp/aobus-dependencies-before.json
./ao check
```

Run that sequence on each affected native host. Keep platform reports distinct,
for example `/tmp/aobus-dependencies-linux-before.json` and
`/tmp/aobus-dependencies-macos-before.json`.

On Windows:

```bat
ao.bat build
ao.bat deps report --json %TEMP%\aobus-dependencies-before.json
ao.bat check
```

The report records the resolved upstream versions and native build identities.
Do not infer the old graph from a new lockfile after the update.

## Updating Nixpkgs

1. Select the intended Nixpkgs revision for Linux.
2. Update the revision, ref, and source hash in `nixpkgs.json`.
3. Compute the unpacked tarball hash rather than copying an unverified value:

```bash
nix-prefetch-url --unpack \
  https://github.com/NixOS/nixpkgs/archive/<revision>.tar.gz
```

4. Enter the new environment through `./ao`. Tooling assertions fail early if
   the package set no longer provides the exact Python, Ruff, or mypy versions
   from `script/ao/toolchain.json`.
5. Build and generate the new dependency report:

```bash
./ao build --clean
./ao deps report --json /tmp/aobus-dependencies-after.json
```

6. Run the clean build, dependency report, and gate on Linux.
7. Review every governed dependency change. The Nix result is a candidate; it
   changes policy only when `dependency-contract.json` is edited explicitly.
8. Review monitor-only changes for API, security, license, patch, and build-option
   risk even though version skew is not a hard failure.

If the Nixpkgs batch changes too many independent high-risk inputs, reduce the
revision jump or split targeted package changes from the package-set update.

## Updating a governed C++ dependency

### 1. Change the contract deliberately

Update the dependency's policy in `dependency-contract.json`. State whether the
policy remains exact or changes to a bounded range. Do not broaden a range only
to make CI green.

### 2. Resolve it on Linux

Normally the accepted version comes from the pinned Nixpkgs package. If the
package set cannot supply the accepted version, either retain the current
contract or add a targeted, source-hash-verified Nix override. Do not use an
ambient package outside `shell.nix`.

Resolve and report the dependency against `nixpkgs.json`.

### 3. Resolve it on macOS and Windows

Use the least forceful shared vcpkg mechanism that preserves the contract:

1. an approved default registry baseline;
2. a direct `version>=` constraint when a minimum is sufficient;
3. a top-level override for a single exact port;
4. a package-scoped registry for a coherent package family;
5. a versioned custom registry when the official registry lacks a maintained
   version;
6. a repository overlay port only as a short-lived emergency measure.

When the default registry baseline changes, update the vcpkg revision, archive
URL, and verified archive SHA-256 in `script/ao/macos-toolchain.json` in the
same change. The macOS tool revision must equal the default registry baseline;
the portal tests enforce that relationship. Compute the archive hash from the
downloaded bytes with `shasum -a 256`; do not copy an unverified digest.

### Recovering a regenerated macOS vcpkg archive

GitHub generates commit archives on demand. The files for a commit remain
stable, but the compressed archive bytes may be regenerated differently and
invalidate the SHA-256 recorded in `script/ao/macos-toolchain.json`.
The mismatch must remain a hard failure until the new archive is independently
verified against the pinned Git commit.

Use a scratch directory outside the repository and substitute the revision and
URL already recorded in the macOS lock:

```bash
AOBUS_VCPKG_AUDIT="$(mktemp -d)"
AOBUS_VCPKG_REVISION="<revision>"
AOBUS_VCPKG_URL="<archiveUrl>"
git init "$AOBUS_VCPKG_AUDIT/repository"
git -C "$AOBUS_VCPKG_AUDIT/repository" remote add origin https://github.com/microsoft/vcpkg
git -C "$AOBUS_VCPKG_AUDIT/repository" fetch --depth=1 origin "$AOBUS_VCPKG_REVISION"
test "$(git -C "$AOBUS_VCPKG_AUDIT/repository" rev-parse FETCH_HEAD)" = "$AOBUS_VCPKG_REVISION"
mkdir "$AOBUS_VCPKG_AUDIT/expected" "$AOBUS_VCPKG_AUDIT/actual"
git -C "$AOBUS_VCPKG_AUDIT/repository" archive FETCH_HEAD \
  | tar -xf - -C "$AOBUS_VCPKG_AUDIT/expected"
curl --fail --location --output "$AOBUS_VCPKG_AUDIT/vcpkg.tar.gz" "$AOBUS_VCPKG_URL"
tar -xzf "$AOBUS_VCPKG_AUDIT/vcpkg.tar.gz" \
  --strip-components=1 -C "$AOBUS_VCPKG_AUDIT/actual"
git diff --no-index --no-ext-diff --exit-code \
  "$AOBUS_VCPKG_AUDIT/expected" "$AOBUS_VCPKG_AUDIT/actual"
shasum -a 256 "$AOBUS_VCPKG_AUDIT/vcpkg.tar.gz"
```

Only after the tree comparison succeeds may the archive SHA-256 change without
changing the pinned revision. Preserve the old cached archive as review
evidence, explain why the upstream bytes changed, then run clean macOS Debug
and Release gates plus `./ao deps verify`. A project-controlled immutable
release asset may replace the generated URL in a later change, but an unhashed
Git clone is not an equivalent supply-chain check.

An override ignores other version constraints. Every new override needs a
reason and an exit condition in the pull-request description. Remove an
override when the normal baseline satisfies the contract.

### Boost

Do not pin only one `boost-*` port. Boost is split into many vcpkg ports, and a
single override can create a mixed release family. Update the Boost-scoped
registry baseline in `vcpkg-configuration.json` to a vcpkg commit whose baseline
contains the contracted Boost release for all selected ports. Keep the scoped
package selection broad enough for `boost*`, `boost-*`, and the `vcpkg-boost`
helper used by current releases.

Verify all Boost library ports installed for Aobus, not only `boost-headers`.
Record recipe helpers such as `vcpkg-boost`, but do not compare their independent
version schemes to the Boost release number.

### Windows App SDK and C++/WinRT

For Windows App SDK or C++/WinRT upgrades:

1. Select the exact top-level releases in `dependency-contract.json`.
2. Regenerate `app/windows-winui/packages.config` with every transitive NuGet
   package and exact version; do not leave transitive selection to an IDE.
3. Confirm `NuGet.Config` still contains the current official nuget.org
   repository signer fingerprints and source mapping.
4. Update the Windows App Runtime identity, installer URL, and SHA-256 in the
   contract when the SDK requires a new runtime.
5. Run `ao.bat doctor winui`, a clean `ao.bat build --target winui`, dependency
   verification for its build tree, and an interactive RDP smoke launch.

The NuGet package graph and installed Windows App Runtime are related but
separate. Updating one without auditing the other is incomplete.

### FTXUI and other single ports

An exact override is acceptable when the target version exists in the selected
registry versions database. Use the version scheme declared by the port, such
as `version-semver` for FTXUI.

### fast_float

Treat the fast_float version and decimal-conversion behavior as one contract.
Update its exact version in `dependency-contract.json`, its source tag and hash
in `shell.nix`, and the shared vcpkg resolution together. Run the `[from-chars]`
regressions on Linux, macOS, and Windows, including representable subnormals,
true underflow and overflow, unchanged output on errors, and ties-to-even
rounding. Do not replace those cases with performance-only evidence.

### spdlog

Treat the spdlog version and formatting backend as one contract. A matching
version is insufficient unless CMake also confirms `SPDLOG_USE_STD_FORMAT` and
rejects `SPDLOG_FMT_EXTERNAL`.

### ICU

Treat ICU data behavior as part of the governed capability contract, not only as a library version and target list.
An ICU upgrade must run the Unicode, catalog, collation, and completion-transliteration fixtures on Linux, macOS, and native Windows.
In particular, rebaseline the fixed Kana and explicitly Mandarin Han aliases because transform output can change with ICU data even when the Aobus source is unchanged.

Run the Release performance review on Linux and native Windows, run the macOS
Release gate, and compare first-use transform construction, the 50,000-track
ASCII snapshot, the 5,155-track CJK-heavy snapshot, and the four cached lookup
shapes where performance baselines exist.
Verify that the dependency report still declares and resolves `script-transliteration`, and exercise both lazy transform constructors so a missing packaged transform is not mistaken for a valid CMake target.

## Updating Python, Ruff, or mypy

`script/ao/toolchain.json` is the version policy source.

1. Update the intended exact versions there.
2. If the Python major/minor changes, update both `pythonFormula` and
   `pythonMajorMinorVersion` in `script/ao/macos-toolchain.json`. The portal
   tests require them to match the major/minor selected by
   `script/ao/toolchain.json`; a patch-only Python update leaves the Homebrew
   lock unchanged.
3. Rebuild `script/ao/windows-requirements.txt` with hashes for every selected
   Windows wheel and transitive package.
4. Rebuild `script/ao/macos-requirements.txt` with hashes for the x86_64 and
   arm64 wheels used by macOS format, tidy, and hygiene commands.
5. Ensure the Linux Nixpkgs set or a targeted Nix derivation supplies the same
   exact versions. macOS must match Ruff and mypy but remains outside the exact
   Python patch-level contract.
6. Run the tooling gate on Linux and Windows:

```bash
./ao test --tooling
```

```bat
ao.bat test --tooling
```

macOS does not expose `./ao test --tooling`; its managed checks, governed C++
dependencies, and native suite gate remain independent.

7. Review new Ruff diagnostics and mypy behavior as policy changes. Keep the
   pin change and any large mechanical cleanup in distinct, understandable
   commits.

Do not change Ruff `target-version` or mypy `python_version` merely because the
managed interpreter patch release changed. Those settings express the minimum
Python language target.

## When vcpkg does not carry the contracted version

Do not silently leave either vcpkg host on another version.

Choose one explicit outcome:

1. keep the contract at the highest version available on every affected
   platform;
2. add a bounded platform exception;
3. add a versioned custom registry for a maintained long-term need;
4. use an overlay port for a short-lived emergency.

A platform exception in `dependency-contract.json` must include a unique ID,
the dependency and platform, allowed version, reason, owner, issue, creation
date, expiry, and exit condition. Normal availability exceptions expire within
30 days. Expired exceptions fail `./ao deps verify`.

## Security updates

The normal Nix lead order does not delay a security fix. Upgrade whichever
platform can consume the fix first, then do one of the following for the lagging
platform:

- consume an upstream or distribution backport;
- disable the affected feature;
- apply a source-hash-verified targeted patch;
- add a bounded exception with compensating controls.

Security-driven skew should normally reconcile within 14 days and sooner when
the vulnerability policy requires it. Even an emergency change must preserve
immutable sources and hashes and pass the minimum clean build and smoke tests.

## Validation for dependency pull requests

Dependency changes need clean Debug and Release validation because stale CMake
and vcpkg state can hide selection and ABI errors.

Linux:

```bash
./ao build --clean
./ao check
./ao check release
./ao deps verify
./ao deps report --json /tmp/aobus-dependencies-linux.json
```

macOS, using guest-local build state:

```bash
./ao build --clean
./ao check
./ao check release
./ao deps verify
./ao deps report --json /tmp/aobus-dependencies-macos.json
```

Windows, in a new build directory or CI cache generation:

```bat
ao.bat build --clean
ao.bat check
ao.bat check release
ao.bat deps verify
ao.bat deps report --json %TEMP%\aobus-dependencies-windows.json
```

Confirm in the report:

- every governed dependency satisfies the contract;
- conditionally disabled dependencies are `not-applicable`, not falsely marked
  as verified;
- required CMake targets and capabilities passed;
- all selected Boost library ports belong to one release family;
- vcpkg port revisions, features, triplet, and registry baselines are present;
- on Linux, the selected Nixpkgs revision and Nix store identities are present;
- no exception is expired or broader than one dependency and platform;
- on Linux and Windows, the actual Ruff and mypy versions match the tooling
  contract; macOS makes no such claim.

The pull request should summarize governed before/after versions and link the
full reports as CI artifacts. Do not paste an unreviewable full transitive graph
into the pull-request body.

## Rollback

Keep the previous known-good pins and dependency reports available through Git
history and CI artifacts. The default rollback is to revert the complete
dependency-alignment change so the contract and every affected resolver remain
coherent.

Do not blindly revert a security update to a known-vulnerable release. If only
one platform must roll back, create a bounded platform exception, record the
security impact, and open the reconciliation work immediately.

## Pull-request checklist

- [ ] The change has one clear dependency/tooling purpose.
- [ ] The before/after governed dependency summary is attached.
- [ ] `dependency-contract.json` changed only when project policy changed.
- [ ] The affected Nix pin and all vcpkg source revisions and hashes are immutable.
- [ ] New vcpkg overrides have a reason and exit condition.
- [ ] Boost ports use one scoped-registry release family.
- [ ] Linux clean Debug and Release gates pass.
- [ ] macOS clean Debug and Release gates pass for cross-platform dependencies.
- [ ] Windows clean Debug and Release gates pass.
- [ ] Dependency reports are retained as CI artifacts.
- [ ] Security and license changes were reviewed.
- [ ] Every platform exception has an owner, issue, expiry, and exit condition.
- [ ] Final contributor/design documentation remains accurate.
