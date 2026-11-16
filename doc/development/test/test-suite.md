---
id: development.test.test-suite
---

# Test suites

Use pinned native Bazel from the repository root for ordinary tests. Follow
[native setup](../bazel.md#direct-bazel-without-the-portal) first; macOS and
Windows require a provisioned SDK, and Windows requires explicit Visual Studio
selectors. For authoring, placement, tags, and helpers, see [testing policy](../test.md).

## Ordinary selection and focused tests

```bash
bazel test //:tests
bazel test //test:library_test --test_filter='TrackStore*'
bazel test //test:uimodel_test --test_filter='Component - behavior*'
bazel test --config=release //:tests
```

`//:tests` is the default ordinary selection. [The root BUILD](../../../BUILD.bazel)
owns its membership and convenience suites such as `//:core_tests`,
`//:application_tests`, `//:query_tests`, and `//:tui_tests`. Platform
compatibility belongs to the member executables; an aggregate does not make a
foreign-platform test available.

- Core tests cover async, audio, library/LMDB, media, query, utility, and
  device-free native-platform contracts. These include portable decoder/graph
  cases and the PipeWire SDK, ALSA null/fake/refusal, Core Audio, and WASAPI
  cases that require no live endpoint or service.
- Application tests cover runtime services and playback, UIModel, catalogs,
  desktop support, the portable WinUI model, performance reporting, media
  fixtures, and runtime fatal-probe scenarios.
- CLI and TUI tests own their frontend contracts.
- Linux `//test:systemmedia_test` owns shared MPRIS adapter contracts and real-TUI
  process cases on owned PTYs/private buses. It is available when system media
  is enabled; real-TUI composition is covered normally and under ASan, not TSan.

Executables own cases by runtime and link needs, not directory; see
[Decision 0021](../../decision/0021-own-test-executables-by-runtime-needs.md).
Use a single owning executable for a focused Catch2 filter. A selected binary
with no matching case fails; an aggregate filter is not a promise to skip
unmatched binaries. Catch2 receives Bazel's `--test_filter` through
`TESTBRIDGE_TEST_ONLY` and its native test setup.

An unescaped comma separates alternative Catch2 filters; shell quotes alone
do not make it literal. Escape commas within a case name:

```bash
bazel test //test:query_test \
  --test_filter='FieldCatalog - finds descriptors by canonical name\, alias\, and field' \
  --test_arg=--list-tests --test_output=all
```

Use name globs for a family of cases. Explicitly exclude hidden probes with
`~[.]` when a filter would otherwise select them. To run retained concurrency
cases in an owning binary, use `--test_filter='[concurrency]~[.]'`; use
`ao check --tsan` for managed whole-scope sanitizer validation.

Bazel owns scheduling, output, result caching, and repetition. For example,
`--runs_per_test=20 --nocache_test_results` requests repeated fresh executions;
it is not the retired wrapper's stop-on-first-failure behavior. Repetition is a
stress aid, not a substitute for deterministic synchronization. `--test_arg`
passes native Catch2 options. Running a previously built executable directly
is diagnosis only: the current checkout does not establish that binary's
source provenance or prepared runtime.

## Complete validation and native prerequisites

`./ao check` (`ao.bat check` on Windows) is complete local and CI validation:
documentation, tooling on Linux/Windows, native completion, optional sanitizer,
and the same scoped check-only hygiene implementation exposed by `ao hygiene`.
`--commit BASE` selects hygiene's diff base, not a smaller native test graph.
See the [completion gate](../bazel.md#completion-gate).

Completion runs `//:check_tests` and the configured `//:check_native_tests`
backend roots. It builds platform products and manual GUI/performance harnesses
without launching those harnesses; Windows also executes the two unattended
native console probes. It gates shared audits and test-source ownership.
Ordinary `bazel test //:tests` does not provide these guarantees.

Explicit native targets retain native prerequisites:

- Linux `//test:gtk_test` needs the owned headless display, private no-activation
  session bus, and GTK environment profile. `ao check` prepares these; a direct
  invocation does not automatically create that context. Never set ownership
  markers on personal desktop endpoints. See [GTK process isolation](uimodel-and-gtk.md#process-isolation-and-diagnostics).
- `//test:backend_test` owns live endpoint/service contracts and PipeWire
  admission cases with five link-interposed stream calls. Hardware and
  channel-map probes marked `[.manual]` stay opt-in. A build does not authorize
  physical playback. Windows currently has only hidden cases and permits an
  empty unattended selection; choose an exact case only after preparing its
  hardware prerequisites.
- AppKit GUI scenarios and WinUI GUI probes are explicit manual executions;
  see [macOS](../macos.md#appkit-smoke-scenarios) and
  [Windows](../windows.md#ui-automation-naming-gate). No test aggregate proves
  an interactive desktop session, physical input, or audible output.

Missing or malformed repository fixtures fail. Only a genuinely optional host
capability may `SKIP()` with a reason; `WARN()` or `SUCCEED()` followed by
`return` is not a skip. The backend PipeWire daemon case skips without a
reachable daemon unless `AOBUS_REQUIRE_PIPEWIRE=1` requires it. Completion
uses private PipeWire/WirePlumber services and null sinks, not host audio.
The device-free cases remain in Core.

On macOS, Core Audio coverage requires a logged-in console user even over SSH.
The portal neither creates a login nor changes power policy; CI checks that
prerequisite. Native macOS has no Python tooling suite and does not claim exact
Homebrew Python patch parity.

## Tooling and lint fixtures

`ao check` runs repository tooling on Linux and Windows. For a standalone Linux
run in the pinned environment:

```bash
nix-shell --run 'PYTHONPATH=script python -m ao.core.tooltest'
```

On Windows the same module requires the already prepared governed Python
and managed tooling environment; use `ao.bat check` unless preparing that
environment explicitly. Do not substitute ambient Python, Ruff, or mypy.
Tooling validates pinned versions and the Windows hash lock, and tests the
document checker against isolated fixtures. The full check also validates the
live documentation tree; `ao docs check` remains its standalone entry.

Tooling's real LLVM diagnostic-filter controls supplement, not replace, the
custom-checker fixtures. Prepare the Windows LLVM SDK explicitly with
`ao.bat deps provision` or `ao.bat setup llvm-sdk`. Missing optional local tools
may skip controls outside CI; under `CI=true`, missing required tools fail.
Configured-tool and version failures never become skips. POSIX controls require
the native LLVM compiler and checker to report the same release version.

Run the lint fixture gate explicitly with:

```bash
bazel test //test/lint:lint_test --nocache_test_results
```

It stays outside `//:tests` but joins completion. Fresh test-result assertions
check the native fixture producer's completed report. That producer checks
`POSITIVE`/`NEGATIVE` markers, optional message assertions, source-policy smokes,
and compiler-error canaries. Clang parses original fixtures; compiler
errors fail without a second syntax-only pass or a GCC compatibility gate.
Producer actions and their reports can be cached; `--nocache_test_results`
does not force that producer to execute anew.

Windows uses the static `AobusClangTidy.exe` host against the provisioned LLVM
SDK, explicit clang-cl arguments, and the selected Visual Studio `INCLUDE`
list, not product PCH flags, ambient `CXX`, or `g++`. Empty include-list entries
are ignored; unused nonexistent entries do not invalidate remaining providers,
but missing required headers fail. Objective-C++ fixtures are explicitly
platform-incompatible on Windows, not counted as tested. Unix uses the governed
plugin/host and macOS SDK/deployment/shim context. Fixture implementation lives
in `script/ao/core/linttest.py` and `script/ao/core/bazel_lint_fixture.py`.
See [fixture authoring](../lint/checker-development.md#fixture-contract).

Coverage remains the separate project-specific `ao coverage` route, with its
own suite selection and unsharded counter collection; see [coverage workflow](coverage-workflow.md).

## Sharded Catch2 execution

Native Bazel and the declared test graph own sharding; there is no project
shard-count environment override or automatic wrapper tally/rerun command.
A Catch2 shard is a complete process with its own fixtures and memory usage;
ASan processes can be expensive, so select concurrency for the host's memory.
Any explicitly prepared GTK shards need separate owned Xvfb displays and
private buses. Sharing one display can let assertions outrun popup X round trips.

When using Catch2's `--shard-count` and zero-based `--shard-index`, all shards
must receive the same numeric `--rng-seed`, or use a shared lexical ordering.
Random ordering with per-process seeds duplicates some cases and omits others
while appearing to partition the suite. Preserve the exact filter, ordering,
seed, index, environment, and binary identity when reproducing a shard.
Do not replay retired display/bus addresses or omit fatal sanitizer settings.
Use `ao check` to recreate managed services; raw reruns are caller-owned and
are not managed completion evidence.
