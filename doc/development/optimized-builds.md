---
id: development.optimized-builds
---
# Optimized builds

Use `release` for optimized correctness and shipping-style evidence. Use
`profile` when sampling needs debug information and frame pointers.

## Choose a flavor

| Flavor | Platforms | Configuration | Intended evidence |
|---|---|---|---|
| `release` | Linux, macOS, Windows | Release with IPO/LTO for supported C++ targets; MSVC uses `/GL`+`/LTCG`, with one exact per-file exception | Optimized build, test, integration, size, and performance validation |
| `profile` | Linux, macOS | Unix `-O2` with debug information and frame pointers; no IPO | Sampling and call-graph investigation |

Every Release configuration enables interprocedural optimization for supported
C++ targets. Debug, ASan, TSan, and `profile` do not enable IPO. Prebuilt
third-party libraries link normally but remain opaque to first-party IPO.

On Linux and macOS, first-party C++ translation units and final links use the
driver's ThinLTO; Objective-C++ translation units retain normal Release
optimization. On Windows, MSVC compiles with `/GL` and links with `/LTCG`;
the exact authored `app/windows-winui/MainWindow.xaml.cpp` translation unit
carries a per-file `/GL-` exception only on the selected MSVC 14.51 toolset,
preserving the existing narrow exception. Generated files and the rest of the
WinUI implementation are not exempt.

Because Bazel 9.2 platform flags replace all previous values of repeatable
flags, the portal explicitly rejects forwarded `--cxxopt`, `--linkopt`, and
`--per_file_copt` in Windows Release before SDK or tree preparation, with a
managed-IPO diagnostic: those platform flags would silently overwrite the
Release `/GL`/`/LTCG` policy rather than merge with caller values. Ordinary
rule copts and the standard compiler baseline flags remain intact, and
Debug/Unix forwarding is not narrowed by this rule. Bare `bazel` users own
their own flags; platform replacement semantics are documented, not merged.

Execution tools and imported SDK tools resolve their own ordinary unprofiled
optimized interfaces independently of a Release or profile target.

IPO does not relax the
[header-definition policy](lint/checker-development.md#header-function-definitions).
It can recover some optimized cross-translation-unit inlining, but it does not
reduce header parsing, Debug/test compilation, or tidy cost.

## Build and run

Linux:

```bash
./ao build release
./ao build release --target aobus-gtk
./ao run gtk release
./ao run tui release
./ao run cli release
./ao build profile
```

macOS:

```bash
./ao build release
./ao run appkit release
./ao run tui release
./ao build profile
```

Windows:

```bat
ao.bat build release
ao.bat build release --target winui
ao.bat run cli release
ao.bat run tui release
ao.bat run winui release
```

WinUI launch needs an interactive desktop session. SSH may build and verify it
but cannot provide the startup smoke.

The portal maps flavors to the managed Bazel profiles: `debug`, `release`,
`asan`, and `tsan` select the configured compilation and SDK modes, and
`profile` selects the Unix sampling profile. Use
`./ao build --help` on the target host for the accepted flavors. An
explicitly selected Bazel tree is mode-neutral — a profile is selected per
invocation — while the per-configuration default trees are named
`bazel-<config>`; the paired provider interfaces follow the selected mode.

`--path`/`-p` and `BUILD_DIR` select one exact tree. Do not point them at a tree
prepared for another checkout. The portal rejects sanitizer profiles combined
with non-Debug flavors.

## Validate optimized behavior

A Release completion gate is the direct IPO evidence:

```bash
./ao check release
./ao hygiene
```

Run those commands on every affected native platform. On macOS,
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
use Debug for ordinary development without presenting it as Release evidence.

IPO links may consume substantially more time and memory than Debug links.
Action caches can reuse frontend work but do not remove whole-program link
cost. Optimized stack traces may contain inlined or eliminated frames.

After a compiler, linker, or toolset change, re-prepare the affected tree so
its repository and action keys observe the new toolchain identity;
`--clean` removes only the tree's Bazel outputs and retains provisioned SDKs.
