---
id: development.optimized-builds
---

# Optimized builds

Use `release` for optimized correctness and shipping-style evidence. Use
`profile` when sampling needs debug information and frame pointers without IPO.
Linux already preserves frame pointers in its
[common compile policy](bazel.md#linux-toolchain-and-modes), including Release.

## Choose a flavor

| Flavor    | Platforms             | Configuration                                                                                              | Intended evidence                                                    |
| --------- | --------------------- | ---------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------- |
| `release` | Linux, macOS, Windows | Release with IPO/LTO for supported C++ targets; MSVC uses `/GL`+`/LTCG`, with one exact per-file exception | Optimized build, test, integration, size, and performance validation |
| `profile` | Linux, macOS          | Unix `-O2` with debug information and frame pointers; no IPO                                               | Sampling and call-graph investigation                                |

Every Release configuration enables interprocedural optimization for supported
C++ targets. Debug, ASan, TSan, and `profile` do not enable IPO. Prebuilt
third-party libraries link normally but remain opaque to first-party IPO.

On Linux, the selected target compiler owns Release IPO: GCC (the default)
uses driver-managed LTO, while Clang (`--compiler=clang`) uses ThinLTO. GCC
release links declare the pinned shell's GNU make helper — a declared GCC
linker input whose `MAKE` environment applies only to release IPO — so
`-flto=auto` keeps its own per-link parallelism instead of a serial
fallback ([make admission and parallelism](bazel.md#release-and-sampling-profiles)).
On
macOS, first-party C++ translation units and final links use the Clang
driver's ThinLTO; Objective-C++ translation units retain normal Release
optimization. On Windows, MSVC compiles with `/GL` and links with `/LTCG`;
the exact authored `app/windows-winui/MainWindow.xaml.cpp` translation unit
carries a per-file `/GL-` exception only on the selected MSVC 14.51 toolset,
preserving the existing narrow exception. Generated files and the rest of the
WinUI implementation are not exempt.

Because Bazel 9.2 platform flags replace all previous values of repeatable
flags, Windows Release platform policy can replace caller `--cxxopt`,
`--linkopt`, and `--per_file_copt` values rather than merge them with its
`/GL`/`/LTCG` policy. Native Bazel callers own their overrides; platform
replacement semantics are documented, not merged. Ordinary rule copts and the
standard compiler baseline remain intact. `ao check` accepts no forwarded
Bazel options.

Execution tools and imported SDK tools resolve their own ordinary unprofiled
optimized interfaces independently of a Release or profile target.

IPO does not relax the
[header-definition policy](lint/checker-development.md#header-function-definitions).
It can recover some optimized cross-translation-unit inlining, but it does not
reduce header parsing, Debug/test compilation, or tidy cost.

## Build and run

Linux:

```bash
bazel build --config=release //app/cli:aobus //app/tui:aobus-tui //app/linux-gtk:aobus-gtk
bazel run --config=release //app/linux-gtk:aobus-gtk
bazel run --config=release //app/tui:aobus-tui
bazel run --config=release //app/cli:aobus
bazel build --config=profile //app/tui:aobus-tui
```

Linux Release defaults to the GCC target compiler; add `--compiler=clang` to
select Clang for that invocation. The compiler selection changes the Release
IPO flavor but not the profile contract above.

macOS:

```bash
bazel build --config=release //app/macos-appkit:aobus-appkit
bazel run --config=release //app/macos-appkit:aobus-appkit
bazel run --config=release //app/tui:aobus-tui
bazel build --config=profile //app/tui:aobus-tui
```

Windows:

```bat
bazel build --config=release //app/windows-winui:aobus-winui
bazel run --config=release //app/cli:aobus
bazel run --config=release //app/tui:aobus-tui
bazel run --config=release //app/windows-winui:aobus-winui
```

WinUI launch needs an interactive desktop session. SSH may build and verify it
but cannot provide the startup smoke.

Use the [pinned native executable and SDK setup](bazel.md#direct-bazel-without-the-portal)
on each host. Bare Bazel defaults to fastbuild; `--config=release` and
`--config=profile` select the named optimized profiles, not merely `-c opt`.
Imported provider interfaces follow each consumer's actual compilation mode.

For project commands such as `check`, `check contracts`, and `measure performance`, `--path`/`-p`
and `BUILD_DIR` select one exact mode-neutral managed tree, and the Linux
target compiler is selected with `--compiler=gcc|clang` (GCC by default). Bare Bazel uses
standard startup output selection instead; it does not interpret those portal
selectors. Do not reuse a prepared rc from another checkout. Sanitizer profiles
require Debug and cannot be combined with Release or sampling.

## Validate optimized behavior

A Release completion gate is the direct IPO evidence:

```bash
./ao check --config release
```

Run that complete check, including its graph-owned lint and formatting tests, on every affected native platform. On macOS,
AppKit GUI scenarios remain separate from `check`; run the applicable scenario
from the Release configuration when native UI behavior changed.

Smoke each affected executable from its Release configuration.

For a runtime-performance claim, measure the same stable workload before and
after in `release`. Use the
[performance review workflow](test/performance.md) when the workload has a
standalone review target. Preserve benchmark output and, when relevant, binary
size, clean-build cost, one-file relink cost, and peak link memory. These are
machine-specific review evidence, not fixed cross-host thresholds.

## Troubleshooting

An unsupported-IPO toolchain is a failed Release build; the compile-contract
verification and Release gates reject missing or contradictory optimized
policy instead of continuing without it. Select a supported compiler/linker or
use explicit Debug or the ordinary fastbuild default for development without
presenting either as Release evidence.

IPO links may consume substantially more time and memory than Debug links.
Action caches can reuse frontend work but do not remove whole-program link
cost. Optimized stack traces may contain inlined or eliminated frames.

After a compiler, linker, or toolset change, re-prepare the affected tree so
its repository and action keys observe the new toolchain identity;
`ao check --clean` removes only that managed tree's Bazel outputs and retains
provisioned SDKs; native Bazel has its own `clean` command.
