# Build and run Aobus

Run repository operations from the root through `./ao` on Linux/macOS or `ao.bat` on Windows.
The portal owns CMake, dependencies, tool selection, and build trees; use `./ao help` and `./ao <command> --help` for command options.

## Linux

The portal re-enters the pinned `nix-shell` automatically.

```bash
./ao build
./ao run tui
./ao check
./ao hygiene
```

`build` performs an incremental debug build. `check` builds and runs the enabled suites in the native `all` group; `hygiene` separately checks the selected source scope.
Choose completion checks from [validation and review](test/validation-and-review.md), not from a second gate list here.

### Linux frontend features

Linux builds enable GTK and native system-media support by default.
Select them independently when constructing a feature matrix:

```bash
./ao build --gtk off --system-media on --target aobus-tui
./ao build --gtk on --system-media off --target aobus-gtk
```

`--gtk on|off` controls `AOBUS_BUILD_GTK`; `--system-media on|off` controls `AOBUS_BUILD_SYSTEM_MEDIA` and the shared `ao_system_media_linux` target used by GTK and TUI.
The same configure options are accepted by `./ao check` and by `./ao run` when those commands build a tree.
`./ao test` does not accept them: it requires an existing configured tree and selects suites enabled there, so use `build`, `check`, or `run` to establish the intended feature tree first. An explicit request for a disabled suite fails rather than running a leftover binary.
Enabling either Linux-only feature on another platform is rejected.

Build selection is separate from the TUI runtime option.
Arguments after `--` belong to the application, for example:

```bash
./ao run tui -- --system-media=off
```

That runtime setting leaves an already built adapter unused; it does not reconfigure the build tree.
Use `./ao build release` for an optimized build; [optimized builds](optimized-builds.md) explains Release, IPO/LTO, and profiling.
`./ao build debug --clean` requests a clean rebuild; preserve a failing tree and its `build.log` when diagnosing a problem.

Linux build trees default to `/tmp/build/<project-directory>`, using the checkout directory's final name.
`AOBUS_BUILD_ROOT` replaces the `/tmp/build` base while retaining that component; an explicit command path or `BUILD_DIR` selects a specific tree.
The portal resolves the same selected tree for tests. Running a Catch2 binary directly is a debugging technique, not the normal workflow.

Portal commands that mutate one build tree serialize through a persistent exclusive lock beside it and report when waiting for another writer.
The adjacent `.ao-build.lock` survives `--clean`; it does not reserve a stable snapshot for running applications, tests, or analysis.
Do not rebuild a tree while relying on it as stable runtime or validation evidence.

The default shell uses Nixpkgs' cached GTK build.
Set `AOBUS_NIX_UNSTRIPPED_GTK=1` only when stepping into GTK internals; it forces a local unstripped GTK build.

## Native macOS and Windows

Use the [macOS guide](macos.md) or [Windows guide](windows.md) before native work: they own host tools, bootstrap, supported commands, build storage, and platform limitations.
Both use the shared governed vcpkg manifest rather than Linux's Nix resolver.
The native AppKit desktop remains a development slice and is launched with `./ao run appkit`; macOS also builds the core, CLI, and TUI with Core Audio.
Windows uses `ao.bat` to initialize the Visual Studio environment and provision the pinned Python tooling.

## Dependencies and optional setup

- [Dependency governance](dependency-governance.md) explains resolver ownership; `./ao deps report` shows governed versions and native identities.
- [Dependency upgrade](dependency-upgrade.md) owns pin changes, including C++, Python, Ruff, and mypy.
- [Concept metrics](concept-metrics.md) owns `./ao deps report --concepts` and its interpretation.
- [Compiler cache](compiler-cache.md) explains explicit setup and shared-workspace opt-in; existing cache environment overrides remain authoritative.
- [Contributor workflow](../../CONTRIBUTING.md) explains optional Git hooks and preparing a review.
