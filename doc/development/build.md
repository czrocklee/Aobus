# Build and run Aobus

Run from the repository root. Use the pinned native `bazel build`, `bazel test`,
and `bazel run` for ordinary development. Project-specific operations use `./ao`
on Linux/macOS or `ao.bat` on Windows; use `./ao help` for their command options.
The [Bazel build system](bazel.md) owns the graph, toolchain, profile, and cache
contracts, including [native executable and SDK setup](bazel.md#direct-bazel-without-the-portal).

## Linux

Install the pinned Bazel version with a JVM and Nix with `nix-shell` on `PATH`.
Bare Bazel needs no pre-entered shell or generated launch rc: its environment
repository prepares the pinned Nix SDK itself.

```bash
bazel build //app/cli:aobus //app/tui:aobus-tui
bazel run //app/tui:aobus-tui
bazel test //:tests
./ao check
```

Bazel builds incrementally and defaults to fastbuild. Linux targets compile with
pinned GCC by default: `--compiler=gcc|clang` on `bazel build`/`test`/`run`
selects the actual target compiler, and the project `check`, `perf` and
`verify` commands accept the same selection (`--clang` remains the Clang
alias). An unknown compiler value fails resolution; there is no silent
fallback. `//:tests` is the ordinary
test selection, not the completion gate. `check` combines documentation,
supported-host tooling, native products/tests and shared audits, optional
sanitizers, and hygiene — complete native C++ lint plus scoped remaining stages;
see the [completion gate](bazel.md#completion-gate).
Standalone `./ao hygiene` uses that same check-only implementation for a selected
source scope. Choose completion checks from
[validation and review](test/validation-and-review.md).

For project commands, the portal enters the pinned `nix-shell` automatically.
It reuses an entered shell only when `AO_NIX_SHELL_FINGERPRINT` matches the
current `shell.nix` inputs and `AOBUS_NIX_UNSTRIPPED_GTK` mode, avoiding doubled
compiler flags. It still selects pinned Python and ignores ambient packages
or an activated virtualenv.

Ordinary Linux fastbuild compiles unoptimized. The
[Bazel mode table](bazel.md#linux-toolchain-and-modes) owns the per-mode
compiler and Nix hardening policy; only optimization-dependent
`fortify`/`fortify3` are excluded in Debug, and other supported hardening
remains.

### Linux frontend features

Linux builds enable GTK and native system-media support by default.
Select them independently when constructing a feature matrix:

```bash
bazel build --//bazel:gtk_enabled=false --//bazel:system_media_enabled=true //app/tui:aobus-tui
bazel build --//bazel:gtk_enabled=true --//bazel:system_media_enabled=false //app/linux-gtk:aobus-gtk
```

The `//bazel:gtk_enabled` and `//bazel:system_media_enabled` boolean build settings
shape compilation and linkage, including the shared `ao_system_media_linux`
target used by GTK and TUI. Disabled products and tests follow configured
compatibility; selecting a label does not restore a disabled feature.
These are Linux graph settings, not switches for macOS or Windows media adapters.

Build selection is separate from the TUI runtime option.
Arguments after `--` belong to the application, for example:

```bash
bazel run //app/tui:aobus-tui -- --system-media=off
```

That runtime setting leaves an already built adapter unused; it does not
reconfigure the build graph.
Use `bazel build --config=release <labels>` for an optimized build and
`--config=profile` for a Unix sampling build; [optimized builds](optimized-builds.md)
explains IPO/LTO and profiling. Native Bazel owns its output selection and
`bazel clean`; neither `--path` nor `BUILD_DIR` selects a bare Bazel output base.
Preserve failing outputs and logs when diagnosing a problem.

### Managed validation trees

Project commands such as `check`, `verify`, `tidy`, and `coverage` use managed
Bazel trees under the platform build root — `/tmp/build/<project-directory>`
on Linux and macOS, using the checkout directory's final name.
Without an explicit selection, each configuration gets its own default tree named
`bazel-<config>` (`bazel-debug`, `bazel-release`, `bazel-asan`, `bazel-tsan`).
`AOBUS_BUILD_ROOT` replaces the `/tmp/build` base while retaining that
component; `--path`/`-p` or `BUILD_DIR` selects one exact tree, and an
explicitly selected tree is mode-neutral: the profile is selected per
invocation, not by the tree, so one explicit tree can host Debug, Release and
managed sanitizer runs.
A native Bazel invocation can use a prepared tree's generated rc explicitly;
see [trees and direct Bazel](bazel.md#trees-and-direct-bazel). Running a test
binary directly from an output tree is a debugging technique, not completion evidence.

Portal commands that mutate one build tree serialize through a persistent
exclusive lock beside it and report when waiting for another writer.
The adjacent lock survives `--clean`; it does not reserve a stable snapshot for
running applications, tests, or analysis.
Do not rebuild a tree while relying on it as stable runtime or validation evidence.

The default shell uses Nixpkgs' cached GTK build.
Set `AOBUS_NIX_UNSTRIPPED_GTK=1` only when stepping into GTK internals; it forces a local unstripped GTK build.

## Native macOS and Windows

Use the [macOS guide](macos.md) or [Windows guide](windows.md) before native work: they own host tools, bootstrap, supported commands, build storage, and platform limitations.
Both use the shared governed vcpkg manifest rather than Linux's Nix resolver.
The native AppKit desktop remains a development slice and is launched with
`bazel run //app/macos-appkit:aobus-appkit`; macOS also builds Core, CLI, and TUI
with Core Audio. Bare native commands require an explicitly selected provisioned
SDK; Windows additionally needs an initialized x64 Visual Studio environment
and all three [standard compiler selectors](bazel.md#direct-bazel-without-the-portal).
Project commands use `ao.bat` to initialize Visual Studio and pinned Python tooling.
SDK installation is explicit: `ao deps provision`, and `ao setup llvm-sdk` for
the Windows LLVM SDK, are the provisioning actions; ordinary build, lint and
format commands never install an SDK and consume a ready installed one.

## Default parallelism

Native Bazel owns its build/test scheduling; use `--jobs=N` and the native
Bazel test scheduling options when needed. It has no project shard-count
environment override.

Project check, tidy, and tooling defaults share the worker baseline in
`script/ao/core/jobs.py`: use available logical cores, reserving one locally but
none on GitHub Actions, with a minimum of one. Discovery respects process
affinity where available; explicit job overrides take precedence. For `ao check`,
`--jobs` also limits the concurrent tooling test modules. Native lint fixture
actions declare their own worker/resource count in the graph. Coverage is the
exception: its `-j` default is a fixed two jobs, not the shared worker baseline.

CI's stdlib-only bootstrap helper computes the same affinity-aware worker count
before native work; an unknown or nonpositive count fails before exports are
written. [Sharded Catch2 execution](test/test-suite.md#sharded-catch2-execution)
explains the native ordering constraints.

## Measure native validation

Set `AOBUS_PHASE_LOG_DIR` to an output directory outside the checkout to collect
opt-in phase measurements while running the normal portal commands:

```bash
AOBUS_PHASE_LOG_DIR=/tmp/aobus-phases ./ao check
```

Each process writes its own JSONL file. This opt-in capture remains a local
tool. CI summarizes its existing completion receipts, BEP files and available
cache stop accounting without enabling these measurements; the immutable raw
artifacts remain available ([CI product
gates](bazel.md#ci-product-gates-and-r2-validation)). Validate collected
records with `python -m ao.core.phases <directory>`; the reporter runs with
`PYTHONPATH=script` and is not a second build or test entry point. Its
optional `--github-summary <file>` appends the same Markdown table. Missing,
malformed, or incomplete measurements fail the report instead of producing an
apparently complete timing result. The complete `check` gate remains mandatory
when required by the change.

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
Command and parent context propagate to worker tasks only when those workers
emit nested phase records; other stage measurements are recorded on the
submitting thread, and separate subprocess sessions do not acquire invented
parent links.
The reporter uses fixed command and suite labels without loading command
parsers or native build modules.

Phase rows can nest or run in parallel; their elapsed times must not be added to
estimate job wall time. These are portal-dispatch measurements: shell bootstrap,
cache actions and other job setup are outside the
recorded command interval. A failed build remains a failed build, while a complete
record of that failure is still a valid measurement. Logging write failures
warn without masking the work's original result; incomplete records remain an
explicit reporting failure. With the variable absent, no timing files are
created and ordinary command output is unchanged. Source IDs are also computed
only when collection is enabled. Record appends remain synchronous and ordered;
encoding happens outside the append lock and directory preparation is amortized.

These measurements attribute existing work. They do not skip configuration,
change cache tracking, reduce suites or lint rules, or establish a
speedup. For private Windows incremental-build evidence, inspect the retained
per-attempt receipts and BEP files; the retired `--msbuild-capture` option is
explicitly rejected with a diagnostic, and no private capture is created.

## Dependencies and optional setup

- [Dependency governance](dependency-governance.md) explains resolver ownership; `./ao deps report` shows governed versions and native identities from the actual installed native and test SDK manifests and hash inputs.
- [Bazel build system](bazel.md): the graph, toolchains, profiles, lint, completion, coverage, and caching contracts behind the native workflow and complete `ao check`, including its full C++ lint stage and scoped remaining hygiene.
- [Dependency upgrade](dependency-upgrade.md) owns pin changes, including C++, Python, Ruff, and mypy.
- [Concept metrics](concept-metrics.md) owns `./ao deps report --concepts` and its interpretation.
- [Contributor workflow](../../CONTRIBUTING.md) explains optional Git hooks and preparing a review.
