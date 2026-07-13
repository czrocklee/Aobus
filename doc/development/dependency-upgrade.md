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

Run every command from the repository root. Linux commands use `./ao`; native
Windows commands use `ao.bat`.

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

On Windows:

```bat
ao.bat build
ao.bat deps report --json %TEMP%\aobus-dependencies-before.json
ao.bat check
```

The report records the resolved upstream versions and native build identities.
Do not infer the old graph from a new lockfile after the update.

## Updating Nixpkgs

1. Select the intended Nixpkgs revision.
2. Update the revision and ref in `nixpkgs.json`.
3. Compute the unpacked tarball hash rather than copying an unverified value:

```bash
nix-prefetch-url --unpack \
  https://github.com/NixOS/nixpkgs/archive/<revision>.tar.gz
```

4. Enter the new environment through `./ao`. Toolchain assertions fail early
   if the new package set no longer provides the exact Python, Ruff, or mypy
   versions from `script/ao/toolchain.json`.
5. Build and generate the new dependency report:

```bash
./ao build --clean
./ao deps report --json /tmp/aobus-dependencies-after.json
```

6. Review every governed dependency change. The Nix result is a candidate; it
   changes policy only when `dependency-contract.json` is edited explicitly.
7. Review monitor-only changes for API, security, license, patch, and build-option
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

### 3. Resolve it on Windows

Use the least forceful vcpkg mechanism that preserves the contract:

1. an approved default registry baseline;
2. a direct `version>=` constraint when a minimum is sufficient;
3. a top-level override for a single exact port;
4. a package-scoped registry for a coherent package family;
5. a versioned custom registry when the official registry lacks a maintained
   version;
6. a repository overlay port only as a short-lived emergency measure.

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

### FTXUI and other single ports

An exact override is acceptable when the target version exists in the selected
registry versions database. Use the version scheme declared by the port, such
as `version-semver` for FTXUI.

### spdlog

Treat the spdlog version and formatting backend as one contract. A matching
version is insufficient unless CMake also confirms `SPDLOG_USE_STD_FORMAT` and
rejects `SPDLOG_FMT_EXTERNAL`.

## Updating Python, Ruff, or mypy

`script/ao/toolchain.json` is the version policy source.

1. Update the intended exact versions there.
2. Rebuild `script/ao/windows-requirements.txt` with hashes for every selected
   Windows wheel and transitive package.
3. Ensure the pinned Nixpkgs set or a targeted Nix derivation supplies the same
   exact versions.
4. Run the tooling gate on both hosts:

```bash
./ao test --tooling
```

```bat
ao.bat test --tooling
```

5. Review new Ruff diagnostics and mypy behavior as policy changes. Keep the
   pin change and any large mechanical cleanup in distinct, understandable
   commits.

Do not change Ruff `target-version` or mypy `python_version` merely because the
managed interpreter patch release changed. Those settings express the minimum
Python language target.

## When vcpkg does not carry the contracted version

Do not silently leave Windows on another version.

Choose one explicit outcome:

1. keep the contract at the highest version available on both platforms;
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
- the Nixpkgs revision and Nix store identities are present;
- no exception is expired or broader than one dependency and platform;
- the actual Ruff and mypy versions match the toolchain contract.

The pull request should summarize governed before/after versions and link the
full reports as CI artifacts. Do not paste an unreviewable full transitive graph
into the pull-request body.

## Rollback

Keep the previous known-good pins and dependency reports available through Git
history and CI artifacts. The default rollback is to revert the complete
dependency-alignment change so the contract and both resolvers remain coherent.

Do not blindly revert a security update to a known-vulnerable release. If only
one platform must roll back, create a bounded platform exception, record the
security impact, and open the reconciliation work immediately.

## Pull-request checklist

- [ ] The change has one clear dependency/tooling purpose.
- [ ] The before/after governed dependency summary is attached.
- [ ] `dependency-contract.json` changed only when project policy changed.
- [ ] Nix and vcpkg source revisions and hashes are immutable.
- [ ] New vcpkg overrides have a reason and exit condition.
- [ ] Boost ports use one scoped-registry release family.
- [ ] Linux clean Debug and Release gates pass.
- [ ] Windows clean Debug and Release gates pass.
- [ ] Dependency reports are retained as CI artifacts.
- [ ] Security and license changes were reviewed.
- [ ] Every platform exception has an owner, issue, expiry, and exit condition.
- [ ] Final contributor/design documentation remains accurate.
