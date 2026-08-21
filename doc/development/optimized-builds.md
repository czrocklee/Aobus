---
id: development.optimized-builds
type: development
status: current
domain: development
summary: Defines Release, IPO, and sampling-oriented profiling workflows.
---
# Optimized builds

## Scope

This guide owns the contributor workflow and role of Aobus optimized build flavors on Linux and Windows.
It distinguishes the IPO-enabled Release graph from sampling-friendly profiling.

## Policy

Choose a flavor by the evidence required:

| Flavor | Platforms | Product optimization | Tests and developer tools | Intended use |
|---|---|---|---|---|
| `release` | Linux and Windows | Release optimization plus IPO/LTO or MSVC LTCG | Tests and every platform-enabled developer target share the IPO-enabled graph | Full optimized build, test, integration, size, and performance validation |
| `profile` | Linux | `RelWithDebInfo` with frame pointers | Included by the normal graph | Sampling and call-graph investigation |

`release` is the single optimized correctness and shipping-style build flavor.
Every enabled first-party target in its Release graph participates in IPO, including product libraries, frontends, and native tests.
Linux also enables its developer tools and lint plugin in that graph; Windows keeps those targets in their governed platform-specific trees.
Configuration verifies C++ IPO support and fails closed when the selected compiler and linker cannot provide it.
IPO applies only to Release configurations.
Prebuilt third-party libraries link normally but remain opaque to IPO.

IPO follows the CMake configuration rather than the portal flavor name.
Every Release configuration enables it, including the Release host build used by Windows tidy.
Debug, ASan, TSan, and the `RelWithDebInfo` `profile` flavor do not enable IPO.
The Windows tidy analysis replay removes MSVC's `/GL` input flag because clang-cl analyzes one translation unit rather than performing link-time code generation; this does not change the Release/IPO configuration of the host lint-plugin build.
Changing compiler or toolset versions requires a clean IPO build because non-fat LTO objects and archives are toolchain-specific.

IPO does not replace the [header-definition lint policy](linting.md#header-function-definitions).
It may recover cross-translation-unit inlining in a Release build, but it does not reduce header parsing, type checking, debug compilation, test compilation, or tidy cost.

## Workflow

Build the complete native Linux Release graph with IPO:

```bash
./ao build release
```

Build or run one Linux frontend:

```bash
./ao build release --target aobus-gtk
./ao run gtk release
./ao run tui release
./ao run cli release
```

Linux also supports a Clang Release tree:

```bash
./ao build release --clang
```

On native Windows, build the complete Ninja Release graph and the WinUI Release configuration:

```bat
ao.bat build release
ao.bat build release --target winui
```

Run from the corresponding Release configuration:

```bat
ao.bat run cli release
ao.bat run tui release
ao.bat run winui release
```

WinUI launch still requires an interactive desktop session.
An SSH session may build it but cannot perform the startup smoke.

The portal routes Release commands to these existing presets and trees:

| Product | Preset | Default tree suffix |
|---|---|---|
| Linux complete native graph | `linux-release` | `release` |
| Windows complete Ninja graph | `windows-release` | `windows-release` |
| Windows WinUI | `windows-winui` | `windows-winui` |

`BUILD_DIR` and `-p` select one exact tree for ordinary commands.
On Windows, the composite `check` command treats that tree as its primary Ninja tree and derives a `-winui` sibling for the required Visual Studio build.
Do not point a Release flavor at a tree configured for a different preset.
The portal rejects sanitizer combinations with `release`.

For sampling with frame pointers:

```bash
./ao build profile
```

## Validation

Release validation itself supplies IPO evidence.
Before accepting a cross-platform optimization or header-placement change, run:

```bash
./ao check
./ao check release
./ao hygiene
```

Also run the Windows counterparts, including `ao.bat check release`, when shared or Windows code is affected.
The Release check builds and tests the native Release graph with IPO and builds the WinUI Release configuration with LTCG.
Smoke each affected executable from its Release tree or configuration.

For a change justified by runtime performance, measure the same stable workload in the normal `release` flavor before and after the change.
Use the [performance-review workflow](test/performance.md) when the workload is covered by the standalone review target.
Record benchmark output together with binary size, clean-build cost, one-file relink cost, and peak link memory when those measurements inform a design or tooling decision.
Machine-dependent build metrics are review evidence rather than fixed cross-host thresholds.

## Troubleshooting

An unsupported-IPO configuration error is a failed Release build, not a reason to continue without IPO in the same tree.
Select a supported compiler and linker; Debug remains available for ordinary development but is not Release evidence.

IPO final links may use substantially more time and memory than Debug links.
Compilation caches can reuse front-end work but do not eliminate the whole-program link.
Optimized stack traces may also contain more inlining and eliminated symbols.

After a compiler, linker, or MSVC toolset update, use a clean `release` tree.
Do not reuse LTO static archives across toolchain versions.

## Related documents

- [Windows development](windows.md) covers native state, WinUI, and interactive launch requirements.
- [C++ coding style](coding-style.md) owns the pure-AST header-definition contract.
- [Linting policy](linting.md) owns checker scope and suppression evidence.
- [Testing policy](test.md) owns test selection.
- [Validation and review](test/validation-and-review.md) owns the completion gate.
