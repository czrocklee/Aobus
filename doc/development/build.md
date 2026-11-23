# Build and run Aobus

Run from the repository root. Use the pinned native `bazel build`, `bazel test`,
and `bazel run` for ordinary development. Project-specific operations use `./ao`
on Linux/macOS or `ao.bat` on Windows; use `./ao --help` for their command options.
The four public commands are `setup` (explicit preparation), `format` (rewriting),
`check` (read-only validation), and `measure` (coverage, performance, and concepts).
The [Bazel build system](bazel.md) owns the graph, toolchain, profile, and cache
contracts, including [native executable and SDK setup](bazel.md#direct-bazel-without-the-portal).
By default, bare Bazel and the portal share one [local action cache](bazel.md#shared-local-action-cache)
across checkouts. Each build tree keeps its own outputs.

## Project commands

| Command                     | Role                                                                                                                                                            |
| --------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `setup <component>`         | Explicit preparation: `bazel`, `deps`, `llvm-sdk`, `winui-runtime`, `buildifier`, `prettier`, or `git-hooks`; each component exposes only its relevant options. |
| `format [paths...]`         | Source rewriting only; paths accept files or directories.                                                                                                       |
| `check`                     | [Full local completion or the native CI selection](bazel.md#completion-gate).                                                                                   |
| `check format [paths...]`   | Read-only formatting; `--docs` selects the global Markdown inventory instead of paths, `--all`, or `--commit`.                                                  |
| `check lint [paths...]`     | Scoped semantic lint and source audits, without formatting or global documentation semantics.                                                                   |
| `check analyzer [paths...]` | Path-sensitive analysis; findings fail unless `--report-only` is explicit.                                                                                      |
| `check docs`                | Global document semantics only; no paths, commit scope, formatting, or native SDK options.                                                                      |
| `check repository`          | [Repository-only CI checks](bazel.md#completion-gate) on Linux.                                                                                                 |
| `check contracts`           | Compile-contract analysis, not product or sanitizer runtime execution.                                                                                          |
| `check deps`                | SDK admission, digest validation, and dependency reporting; optional JSON output.                                                                               |
| `check winui`               | Read-only WinUI prerequisites; `--build-only` omits runtime prerequisites.                                                                                      |
| `check instrumentation`     | Linux Debug instrumentation analysis only; no measurement or test execution.                                                                                    |
| `measure coverage`          | Actual Linux Debug coverage collection; select suites with `--suite`.                                                                                           |
| `measure performance`       | Performance review, defaulting to Release; select other supported modes with `--config`.                                                                        |
| `measure concepts`          | Dependency concept metrics with SDK verification and provenance.                                                                                                |

Scoped source commands accept `--all` or `--commit REV` instead of paths.
Missing paths and invalid selections fail before discovery or native preparation.
`format` and `check format` require ready tools; use explicit `setup` when a tool
is absent. `--compiler gcc|clang` selects the Linux product compiler where
supported. Use each command's `--help` for its own options rather than passing
native build, sanitizer, or SDK switches to unrelated checks or measurements.

## Linux

Install the pinned Bazel version with a JVM and Nix with `nix-shell` on `PATH`.
Bare Bazel needs no pre-entered shell or generated launch rc: its environment
repository prepares the pinned Nix SDK itself. Tests that need a private session
bus, audio server or display start it with tools from that SDK. Those tools need
not be on your `PATH`; see [direct Bazel](bazel.md#direct-bazel-without-the-portal)
for prerequisites.

```bash
bazel build //app/cli:aobus //app/tui:aobus-tui
bazel run //app/tui:aobus-tui
bazel test //:tests
./ao check
```

Bazel builds incrementally and defaults to fastbuild. Linux targets compile with
pinned GCC by default: `--compiler=gcc|clang` on `bazel build`/`test`/`run`
selects the actual target compiler. Project `check`, `measure performance`, and
`check contracts` accept `--compiler gcc|clang`; there is no `--clang` alias. An unknown compiler value fails resolution; there is no silent
fallback. Use `//:tests` for the ordinary test selection and `./ao check` for the [completion gate](bazel.md#completion-gate).
Standalone `./ao check lint` validates source semantics in a selected scope;
`./ao check format` verifies formatting separately. Choose completion checks from
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

Project commands such as `check`, `check contracts`, `check lint`, and
`measure coverage` use managed
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
To launch the AppKit development slice without using personal settings or library
data, follow the [isolated AppKit launch](macos.md#bazel-appkit-product).
macOS also builds Core, CLI, and TUI with Core Audio. Bare native commands require an explicitly selected provisioned
SDK; Windows additionally needs an initialized x64 Visual Studio environment
and all three [standard compiler selectors](bazel.md#direct-bazel-without-the-portal).
For shell-backed Windows actions, follow the same guide to select Git Bash in
the client environment.
Project commands use `ao.bat` to initialize Visual Studio and pinned Python tooling.
SDK installation is explicit: `ao setup deps`, and `ao setup llvm-sdk` for
the Windows LLVM SDK, are the provisioning actions; ordinary build, lint and
format commands never install an SDK and consume a ready installed one.

### Shared-source Git discovery

If native Git cannot follow a foreign linked worktree's `.git` pointer, set `AOBUS_GIT_DIR` to the private Git directory and `AOBUS_GIT_WORK_TREE` to the source root seen by the portal for that invocation. Use the [macOS shell example](macos.md#bind-git-metadata-on-macos) or the [Windows PowerShell example](windows.md#bind-git-metadata-on-windows). Bind existing metadata; do not create or copy it for these examples.

Before using the pair, compare the private directory, common directory, index path, exact HEAD, and staged entries with the intended checkout. Matching metadata does not verify the contents of a source mirror or its comparison base. Check the intended edits and the base revision separately. A matching `--show-toplevel` is not sufficient: the portal trusts the association. Both values must be nonempty native absolute paths. Discovery rejects a malformed pair and ignores a complete pair for a different normalized root; use the portal's spelling, not an assumed path alias. If the changed-files comparison fails because Git returned an error after ignoring the pair, the diagnostic names both roots.

Name files individually to bypass Git-backed scope discovery. For `format`, `check format`, `check lint`, and `check analyzer`, directories and `--all` skip the changed-files comparison but can still call Git to enumerate authored text files. That enumeration falls back to a filesystem scan only if Git is missing or reports no repository and no effective binding selects this checkout. A malformed pair or a binding for this checkout that Git cannot use fails instead.

`--commit REV` requires Git and selects a base while still including staged, unstaged, and non-ignored untracked changes. A change only to timestamps or other cached file stats does not select a file; real content changes and applicable executable-bit changes still count. Discovery uses both `--no-optional-locks` and `-c diff.autoRefreshIndex=false` because optional-lock suppression alone does not prevent diff index refreshes.

Only Git-backed source discovery uses the binding. It does not rewrite `.git`, redirect `setup git-hooks`, performance provenance, arbitrary Git commands, or checkout-ID creation. If managed tools cannot access the original private directory, supply a separate stable, unique `AOBUS_CHECKOUT_ID`. Do not export standard `GIT_DIR` or `GIT_WORK_TREE` across the portal: dependencies and disposable test repositories need independent Git contexts. These safeguards do not make the whole checkout write-free.

## Default parallelism

Native Bazel owns its build/test scheduling; use `--jobs=N` and the native
Bazel test scheduling options when needed. It has no project shard-count
environment override.

Project completion, lint, and tooling defaults share the worker baseline in
`script/ao/core/jobs.py`: use available logical cores, reserving one locally but
none on GitHub Actions, with a minimum of one. Discovery respects process
affinity where available; explicit job overrides take precedence. For `ao check`,
`--jobs` is Bazel's job count, so it also bounds the concurrently executing
tooling targets. Native lint fixture
actions declare their own worker/resource count in the graph. Coverage and
instrumentation use the same baseline for `-j`; coverage collection retains
its separate memory-specific limits.

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
tool. CI summarizes its existing check BEP files and available
cache stop accounting without enabling these measurements; see
[CI evidence](bazel.md#ci-product-gates-and-r2-validation) for the retained artifacts.
Validate collected records with `python -m ao.core.phases <directory>`; the
reporter runs with `PYTHONPATH=script` and is not a second build or test entry point. Its
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

- [Dependency governance](dependency-governance.md) explains resolver ownership; `./ao check deps` validates and shows governed versions and native identities from the actual installed native and test SDK manifests and hash inputs.
- [Bazel build system](bazel.md): the graph, toolchains, profiles, lint, completion, coverage, and caching contracts behind the native workflow and complete `ao check`, including its whole-graph lint test.
- [Dependency upgrade](dependency-upgrade.md) owns pin changes, including C++, Python, Ruff, and mypy.
- [Concept metrics](concept-metrics.md) owns `./ao measure concepts` and its interpretation.
- [Contributor workflow](../../CONTRIBUTING.md) explains optional Git hooks and preparing a review.
