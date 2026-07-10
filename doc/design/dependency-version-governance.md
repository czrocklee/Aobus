# Dependency Version Governance

## Status

This document describes the current dependency-version policy for Aobus. The
developer procedure for changing pins is in
`doc/dev/dependency-upgrades.md`.

## Goals

Aobus resolves dependencies through different native ecosystems:

- pinned Nixpkgs on Linux;
- versioned vcpkg registries on Windows;
- a managed Python environment for repository tooling.

The ecosystems are not expected to produce identical transitive graphs. The
governance goal is narrower: selected direct dependencies must satisfy one
project contract on every supported host, and every platform-specific
resolution must remain reproducible and auditable.

## Sources of Truth

Each file owns a distinct question:

| Question | Source of truth |
|---|---|
| Which C++ dependency versions and capabilities does Aobus accept? | `dependency-contract.json` |
| Which Nixpkgs package set does Linux resolve from? | `nixpkgs.json` |
| Which vcpkg registry snapshots does Windows resolve from? | `vcpkg-configuration.json` |
| Which vcpkg ports and features does Aobus consume? | `vcpkg.json` |
| Which Python, Ruff, and mypy versions does repository tooling require? | `script/ao/toolchain.json` |
| Which Windows Python artifacts and transitive packages are accepted? | `script/ao/windows-requirements.txt` |
| Did the configured build satisfy the contract? | CMake dependency checks and `./ao deps verify` |

The Nixpkgs pin is the normal **lead resolver**, not the policy source of truth.
A routine upgrade normally starts by evaluating a new Nixpkgs pin, but the
resolved versions become project policy only after an explicit
`dependency-contract.json` change is reviewed and both native builds pass.

## Governed Dependencies

The initial governed set is deliberately small:

- Boost;
- FTXUI;
- spdlog.

The contract records the alignment policy, native package mappings, required
CMake targets, build-option condition, and behavior-affecting capabilities.
CMake reads the contract before dependency discovery and fails during configure
when an active governed package does not satisfy it. A conditionally disabled
dependency is reported as `not-applicable`, not as verified or missing.

Other direct and transitive dependencies are reported for visibility but are
not required to have the same upstream version across operating systems. A
dependency should be promoted into the governed set only when at least one of
these conditions applies:

- observed API or behavior drift has caused cross-platform failures;
- templates, configuration macros, or public types create an ABI or ODR risk;
- the dependency is security-sensitive;
- the cost of maintaining an explicit contract is lower than the recurring
  diagnosis cost.

## Alignment Is Not Build Identity

Cross-platform alignment compares normalized upstream versions and required
capabilities. It does not claim that Nix and vcpkg build identical artifacts.

Platform build identity includes additional information:

- Nixpkgs revision, Nix output path, compiler, overlays, and downstream patches;
- vcpkg registry baseline, upstream version, port version, features, triplet,
  overlays, and tool version.

For example, vcpkg `spdlog 1.17.0#1` satisfies an upstream version contract of
`1.17.0`; the `#1` port revision remains part of the Windows build identity and
must still appear in the dependency report.

Version equality is never used as proof of ABI or behavior equality. The build
also verifies required imported targets and behavior-affecting options. In
particular, Aobus requires spdlog to advertise `SPDLOG_USE_STD_FORMAT` and to
avoid `SPDLOG_FMT_EXTERNAL`.

## Native Resolution

### Linux

`shell.nix` imports the exact Nixpkgs revision and source hash from
`nixpkgs.json`. It verifies that the Nix package versions for Python, Ruff, and
mypy match `script/ao/toolchain.json` during evaluation. The shell also exposes
a generated dependency report containing versions and Nix store identities.

An unrelated package from `PATH`, a channel, or `NIX_PATH` is not an accepted
substitute for the environment entered by `./ao`.

### Windows

`vcpkg-configuration.json` owns registry identities. Its default registry
selects the normal port graph. Dependencies that need a coherent historical
family can use a package-scoped registry baseline.

Boost uses a scoped registry because vcpkg publishes Boost as many related
`boost-*` ports. Pinning only one Boost port with a manifest override would
allow mixed Boost release families. The scoped registry keeps every selected
Boost port on one release. Its package selection also includes the
`vcpkg-boost` helper used by current Boost releases. Recipe helpers remain
pinned by that registry but are excluded from Boost library release-family
equality because they use their own version schemes.

Manifest overrides are reserved for a small number of single-port, exact pins.
An override ignores other version constraints and therefore requires an
explicit reason and removal condition. It is not the default update mechanism.

The Windows dependency report preserves the complete
`version#port-version`, selected features, target triplet, registry baselines,
and vcpkg tool version.

## Tooling Contract

`script/ao/toolchain.json` is platform-neutral even though Windows performs the
managed bootstrap. Linux Nix evaluation and the Windows checkout environment
must both match its exact Python, Ruff, and mypy versions.

`script/ao/windows-requirements.txt` is an artifact lock rather than a second
policy file. It pins Windows wheels and transitive packages by hash. Tooling
tests verify that its Ruff and mypy entries agree with the toolchain contract.

The Ruff target version and mypy `python_version` in `pyproject.toml` describe
the Python language compatibility target. They do not need to equal the exact
development interpreter patch release.

## Enforcement

The enforcement path is layered:

1. Nix evaluation or the Windows Python bootstrap validates the tooling
   contract.
2. CMake reads `dependency-contract.json`, selects the effective host policy or
   active platform exception, rejects expired exceptions, and performs
   exact/range package discovery.
3. CMake validates required imported targets and dependency capabilities.
4. CMake writes `aobus-dependencies.json`, including the contract SHA-256 and
   active/not-applicable state, in the build directory.
5. `./ao deps verify` checks report freshness, native resolver identity, and
   active exception policy.
6. `./ao check` runs dependency verification after the build and before native
   and tooling tests.

Generated reports are build artifacts. They are not committed to the source
tree. CI retains the full JSON report for dependency-changing pull requests and
prints a concise before/after summary for review.

## Exceptions

Silent version skew is not allowed. A temporary platform difference must be an
entry in `dependency-contract.json` with:

- a unique ID;
- one dependency and one platform;
- the temporarily allowed version;
- a technical reason and risk statement;
- an owner and reviewing issue;
- creation and expiry dates;
- an exit condition.

Expiry uses a UTC calendar date and fails closed. A normal ecosystem-availability
exception should not exceed 30 days. Security-driven platform skew should be
reconciled within 14 days unless the security policy requires a shorter window.

An exception may narrow one dependency check. It may not disable dependency
verification for an entire platform.

## Security Updates

Nix leads routine upgrades, not emergency response. A security fix may land on
the first platform that can consume it. The same change must update the contract
or add a bounded exception for the lagging platform, include compensating
controls when applicable, and open the reconciliation work.

Urgency does not permit mutable sources, missing hashes, or skipping the minimum
clean build and smoke tests.

## Upgrade Atomicity

There are three update classes:

- A Nixpkgs pin update is a batch change by nature. It must include a generated
  diff for governed and monitored dependencies.
- A normal governed-dependency update changes the contract and both native
  resolution paths in one pull request.
- An emergency platform-first update may use a bounded exception and is not
  blocked by the normal Nix lead order.

Atomicity means the default branch remains in one verified state. It does not
require unrelated tool upgrades or code formatting changes to share one commit.

## Non-goals

This policy does not:

- require identical Linux and Windows transitive dependency graphs;
- compare vcpkg port revisions with Nix package metadata;
- promise cross-operating-system ABI identity;
- promise bit-for-bit reproducible binaries;
- require every dependency to become governed;
- require flakes or a particular CI provider.
