---
id: development.optimized-builds
---
# Optimized builds

Use `release` for optimized correctness and shipping-style evidence. Use
`profile` when sampling needs debug information and frame pointers.

## Choose a flavor

| Flavor | Platforms | Configuration | Intended evidence |
|---|---|---|---|
| `release` | Linux, macOS, Windows | `Release` with IPO/LTO for supported C++ targets; WinUI uses LTCG | Optimized build, test, integration, size, and performance validation |
| `profile` | Linux, macOS | `RelWithDebInfo` with frame pointers; no IPO | Sampling and call-graph investigation |

Every Release CMake configuration enables interprocedural optimization and fails
at configure time if the selected C++ compiler/linker cannot provide it. Debug,
ASan, TSan, and `profile` do not enable IPO. Prebuilt third-party libraries link
normally but remain opaque to first-party IPO.

On Linux, all enabled first-party Release targets share the IPO graph. Windows
uses IPO in the native Ninja Release graph and LTCG in the WinUI Release
configuration. On macOS the IPO probe is deliberately C++-only: C++ translation
units and final links use ThinLTO, while Objective-C++ translation units retain
normal Release optimization.

IPO follows the CMake configuration, not merely the portal flavor spelling. The
Windows tidy host tree inherits the Release preset, but analysis replay removes
MSVC `/GL` because clang-tidy analyzes one translation unit; that does not
change the host tree's Release configuration.

Changing compiler, linker, or MSVC toolset requires a clean Release tree because
non-fat LTO objects and archives are toolchain-specific.

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
./ao build release --clang
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

The portal maps flavors through the platform profile and `CMakePresets.json`:
Linux uses `linux-release`, macOS uses `macos-release`, the native Windows graph
uses `windows-release`, and WinUI uses the multi-config `windows-winui` tree.
Linux and macOS profiling select their platform's profile preset. Use
`./ao build --help` on the target host for the accepted flavors.

`BUILD_DIR` and `-p` select one exact primary tree. On Windows, a composite
`check` derives a `-winui` sibling when the primary path is explicit. Do not
reuse a tree configured for another preset. The portal rejects sanitizer flags
with non-Debug flavors.

## Validate optimized behavior

A Release check is the direct IPO evidence:

```bash
./ao check
./ao check release
./ao hygiene
```

Run those commands on every affected native platform. On Windows,
`ao.bat check release` also builds the WinUI Release configuration. On macOS,
AppKit GUI scenarios remain separate from `check`; run the applicable scenario
from the Release tree when native UI behavior changed.

Smoke each affected executable from its Release tree or configuration.

For a runtime-performance claim, measure the same stable workload before and
after in `release`. Use the
[performance review workflow](test/performance.md) when the workload has a
standalone review target. Preserve benchmark output and, when relevant, binary
size, clean-build cost, one-file relink cost, and peak link memory. These are
machine-specific review evidence, not fixed cross-host thresholds.

## Troubleshooting

An unsupported-IPO configure error is a failed Release build; do not continue in
that tree without IPO. Select a supported compiler/linker or use Debug for
ordinary development without presenting it as Release evidence.

IPO links may consume substantially more time and memory than Debug links.
Compiler caches can reuse frontend work but do not remove whole-program link
cost. Optimized stack traces may contain inlined or eliminated frames.

After a compiler, linker, or toolset update, clean the Release tree rather than
reusing old LTO archives.
