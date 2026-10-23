# Build and run Aobus

Run repository operations from the root through `./ao` on Linux/macOS or `ao.bat` on Windows.
The portal owns CMake, dependencies, tool selection, and build trees; use `./ao help` and `./ao <command> --help` for command options.

## Linux

The portal enters the pinned `nix-shell` automatically. It reuses an already entered
shell, such as a direnv `use nix` environment, only when that shell's
`AO_NIX_SHELL_FINGERPRINT` matches the current `shell.nix` inputs and
`AOBUS_NIX_UNSTRIPPED_GTK` mode; nesting another `nix-shell` would pass every
compiler flag twice. Reuse still selects the pinned Python environment and
ignores ambient Python packages or an activated virtualenv.

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
Enabling GTK on another platform is rejected.
`--system-media` is rejected on other platforms with either value: their desktop media adapters are not switchable yet, so those trees keep no `AOBUS_BUILD_SYSTEM_MEDIA` cache entry, and configure removes a leftover `OFF` entry.

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

## Measure native validation

Set `AOBUS_PHASE_LOG_DIR` to an output directory outside the checkout to collect
opt-in phase measurements while running the normal portal commands:

```bash
AOBUS_PHASE_LOG_DIR=/tmp/aobus-phases ./ao check
```

Each process writes its own JSONL file. CI enables this on native jobs, validates
the records with `python -m ao.core.phases <directory>`, and publishes normalized
`AO_PHASE_RECORD` rows in the job log plus a GitHub step-summary table. The
reporter uses the job's Python environment with `PYTHONPATH=script`; it is not a
second build or test entry point. Its optional `--github-summary <file>` appends
the same Markdown table. Missing, malformed, or incomplete measurements fail the
report instead of producing an apparently complete timing result. CI summary
steps are best-effort: reporting failures remain visible but do not fail an
otherwise successful native job. Build and hygiene gates remain mandatory.

Records contain fixed phase/command labels, process and nesting identities,
monotonic durations, exit statuses, and relevant numeric scope/concurrency
counts. They do not contain command lines, environment values, host paths,
test filters, or exception messages. Analysis-item records identify a selected
repository file by the lowercase SHA-256 of its project-relative POSIX spelling
encoded as UTF-8; the digest can be reproduced from a known tracked path without
publishing a host-specific path. It is an identifier, not secret anonymization.

The summary distinguishes phase `records` from processed `items`. Items are
summed only when every record in a group supplies a work count; a dash means
unspecified or partial counts. Counts from nested phases are not unique inputs.
Worker tasks inherit the submitting command and phase within one process;
separate subprocess sessions do not acquire invented parent links. Command and
suite identities come from lightweight registries, so the reporter does not
load command parsers or native build modules.

Phase rows can nest or run in parallel; their elapsed times must not be added to
estimate job wall time. These are portal-dispatch measurements: shell bootstrap,
compiler-cache activation, cache actions and other job setup are outside the
recorded command interval. A failed build remains a failed build, while a complete
record of that failure is still a valid measurement. Logging write failures
warn without masking the work's original result; incomplete records remain an
explicit reporting failure. With the variable absent, no timing files are
created and ordinary command output is unchanged. Source IDs are also computed
only when collection is enabled. Record appends remain synchronous and ordered;
encoding happens outside the append lock and directory preparation is amortized.

`ao run` uses a bounded `launch-preparation` phase rather than leaving a root
command span open across `exec`. Preparation success is not proof of successful
handoff, application runtime, or application exit status. Failed handoff or run
workspace setup/cleanup produces `launch-error` evidence, measuring failure
handling rather than application runtime. An `application` phase
observes runtime/status only on the existing Windows source-view path where the
portal already waits for its child. This measurement does not replace `exec`
with a supervising process or retire source views while the application runs.

These measurements attribute existing work. They do not skip configuration,
change compiler-cache tracking, reduce suites or lint rules, or establish a
speedup. For private Windows incremental-build evidence, use the
[MSBuild capture procedure](windows.md#capture-msbuild-incrementality).

## Dependencies and optional setup

- [Dependency governance](dependency-governance.md) explains resolver ownership; `./ao deps report` shows governed versions and native identities.
- [Dependency upgrade](dependency-upgrade.md) owns pin changes, including C++, Python, Ruff, and mypy.
- [Concept metrics](concept-metrics.md) owns `./ao deps report --concepts` and its interpretation.
- [Compiler cache](compiler-cache.md) explains explicit setup and shared-workspace opt-in; existing cache environment overrides remain authoritative.
- [Contributor workflow](../../CONTRIBUTING.md) explains optional Git hooks and preparing a review.
