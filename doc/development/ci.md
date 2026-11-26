# Continuous integration and Release validation

Use this guide to identify which GitHub Actions jobs should run, reproduce a
lane locally, or find evidence after a failure. Use [validation and review](test/validation-and-review.md#validation)
to choose the checks required before delivery; a passing PR job does not replace
required local or native-host validation.

## CI routing

[CI](../../.github/workflows/ci.yml) runs for PRs to `main`, pushes to `main`, and
manual dispatches. `Change scope` verifies the event's comparison base and
classifies the final tree against it. For PRs the base must be the event's exact
base commit SHA and an ancestor of the checked-out commit.

A documentation-only comparison must change at least one path, and every
changed path must be `AGENTS.md`, `CONTRIBUTING.md`, `README.md`, or a Markdown
file under `doc/`, `.agents/skills/`, `tool/`, `app/`, or `asset/`. The local
`doc/plan/` tree is ignored and outside documentation discovery. Renames count
as deletion plus addition, so moving code into a Markdown path still triggers
native validation.
Deleting the WinUI deployment `README.md` or changing its file type also
triggers native validation: the package needs that file. Empty comparisons,
other paths, and manual dispatches take the native route.

- **Documentation-only route:** `Repository / check` runs `ao check docs` and
  `ao check format --docs` with pinned host Python and the
  [governed parser and formatter](dependency-governance.md#repository-tooling),
  without Nix, Bazel, or a native SDK. Native jobs are skipped.
- **Native route:** `Repository / check` runs `./ao check repository` on Linux
  in parallel with `Linux / native`, `macOS Intel / check`,
  `macOS ARM64 / check`, and `Windows / check`. The native jobs run fastbuild
  and use the selections below.

`Linux / check` preserves the required status as a joint verdict, not another
build. It requires successful classification and a passing repository job,
plus a passing `Linux / native` job on the native route or a skipped one on
the documentation-only route. Native jobs depend on scope, not the repository
result. Repository rulesets decide which checks block merging. A build-system migration still
requires successful results from the full affected native matrix.

## Reproduce the native jobs

Run in a checkout matching the workflow's revision, with the
[native prerequisites](bazel.md#direct-bazel-without-the-portal). Replace `SHA`
with the verified comparison commit from the `Change scope` log. The table
omits runner-specific build paths, job counts, and cache options.

| Event                                    | Linux and macOS                            | Windows                                                      |
| ---------------------------------------- | ------------------------------------------ | ------------------------------------------------------------ |
| PR, native route                         | `./ao check --native-only --lint-base SHA` | `ao.bat check --native-only --desktop-tests --lint-base SHA` |
| Push to `main` or manual **CI** dispatch | `./ao check --native-only --extend`        | `ao.bat check --native-only --desktop-tests --extend`        |

Each native job logs the production lint scope and the reason for its
selection. PRs gate production clang-tidy on changed native files, not on
unchanged consumers of a BUILD, toolchain, or lint-rule change. Buildifier
formatting and semantic lint, native builds and tests remain required. Native
pushes to `main` use `--extend` to catch findings in unchanged files. See the
[completion gate](bazel.md#completion-gate) for fallback rules and suite selections. Windows desktop tests require an
[interactive console and runtime](windows.md#ui-automation-naming-gate);
macOS Core Audio tests require a [logged-in console user](macos.md#validation).

Before native work, CI's standard-library bootstrap helper exports the
[affinity-aware job count](build.md#default-parallelism). An unknown or
nonpositive count fails before the helper writes exports.

## Manual Release validation

With the [Release workflow](../../.github/workflows/release.yml) on the
default branch, select the branch to validate under **Actions → Release
validation → Run workflow**. Linux, macOS Intel and ARM64, and Windows run
`./ao check --extend --config release` (on Windows,
`ao.bat check --extend --config release`). This is the full Release selection,
not the fastbuild native partition. The Windows desktop UI Automation gate
runs in CI, not Release validation.

Release uses normal Bazel caching within each job but does not restore caches
from earlier workflow runs. For a slow Windows run, compare dependency
preparation and **Check Release** in the job timings. On failure, download
the job's diagnostics artifact. The workflow does not sign or publish products,
tag commits, or create a release.

## Validation evidence

After Linux native completion passes, a [scoped lint build](../../test/lint/check_scoped_reports.py)
checks that bare Bazel can bootstrap and produce a usable paired-header report
without a portal launch rc or R2. It can reuse the disk cache. It does not
run a bare `TestRunner`; to verify that boundary, run
`bazel test --nocache_test_results //:tests` with the native prerequisites.

On changes to checker forwarding, its probe, or its CI route, Linux and Windows
also run the [mixed checker report probe](../../test/lint/check_checker_reports.py).
To reproduce it after native completion, set `PYTHONPATH=script` and run
`python test/lint/check_checker_reports.py <prepared-tree>` from the checkout
with the pinned host Python (inside the pinned Nix shell on Linux). It compares
the delivered Debug request with the native compiler arguments and objects for
the verified tool and the product source. It does not run whole-graph lint.

Use the **Check Bazel completion** step's exit status for the completion
verdict. A passing Bazel test graph or report build does not prove scoped
lint passed: the portal reads the reports and can still reject a finding or
missing context.

For offline diagnosis, open the step transcript in the native job artifact:
`aobus-completion.log` on Linux and macOS, `completion.log` on Windows. It
includes the diagnostic text and identifies the test attempt and any scoped
lint run that started. The artifact also includes per-target test logs and
`lint/run-*` pass logs, BEPs, and receipts when produced. `run.receipt.json`
records the selection, not the verdict; early failures may have no receipt.

The best-effort job summary reads test verdicts and target-level counts from
the check attempt's Build Event Protocol (BEP) stream. It also lists timing
and action-cache counters per invocation and available R2 stop accounting.
Missing or incomplete evidence is not a zero count or a pass. Nested times
overlap. Bazel's local incremental action-cache counters, execution runners,
test-result reuse, and whole-job R2 blob transfers are distinct; none measures
an R2 hit rate. The summary reads existing evidence without adding capture,
cache probes, or gates; it does not replace the artifacts. For local timing
comparisons, see [phase capture](build.md#measure-native-validation).

## Workflow caches

Only `Linux / native` archives the Bazel disk cache between runs. The portal,
the bare Bazel lint build, and cache steps share `$RUNNER_TEMP/aobus-state/cache/bazel`;
Windows uses a job-local `D:\aobus-bazel-cache`. OS, architecture, pins, and
commit identify candidate archives, with fallback to the same pin contract.
Bazel action keys still decide reuse. PRs can restore main's archive subject
to GitHub's cache access rules; only pushes to `main` can save and only after
native completion and the bare lint smoke pass.

A marker outside the disk cache records when restore finished. Exact hits
need no trim or save. Before saving, trim removes only `ac/` and `cas/` files
whose modification times are not newer than the marker. Bazel updates these
times on access, so trim retains the run's working set rather than imposing a
size limit or waiting for idle garbage collection. Restore and save include
only `ac/` and `cas/`; `gc/lock` and `tmp/` stay on the runner. A
failed restore, marker, or trim prevents saving, not validation.

The macOS and Windows jobs restore independently keyed vcpkg and managed-tool
archives. They seal the vcpkg archives after provisioning so an admitted cache
fill can survive a later validation failure. They save managed tools only after
preparation checks pass. Windows keys the LLVM SDK separately and verifies its
completion marker before saving.

## R2 validation

With `AOBUS_BAZEL_R2_CACHE_AUTHORIZED=true`, trusted main pushes write the shared
`bazel-v1/` prefix. Same-repository PRs consume it with proxy uploads disabled.
Fork PRs have no R2 binding, and secretless runs keep their local caches.
Ordinary local commands retain the [Bazel disk-cache default](bazel.md#shared-local-action-cache).

A manual **CI** dispatch with `r2=true` trusts the selected checkout and uses a
run/attempt-isolated `bazel-ci-validation/` prefix. It requires a nonempty
proxy URL and a successful drain; falling back to local cache cannot pass.
It uses the `R2_SCCACHE_ENDPOINT`, `R2_SCCACHE_ACCESS_KEY_ID`, and
`R2_SCCACHE_SECRET_ACCESS_KEY` secrets and the `aobus-sccache` bucket. No other
bucket or credential is required. Fork and secretless runs cannot pass live
validation.

Upload-disabled PR consumers still use the same bucket-level writer credentials,
not a backend read-only token. Prefix separation, workflow conditions, and
disabling upload workers are **not backend authorization**. A malicious
same-repository workflow edit can still address repository secrets; this route
does not isolate that threat.

The [cache helper](../../script/ao/portal/bazel_remote_cache.py) downloads and
verifies the proxy pinned by version and SHA-256 in
[`bazel-remote.json`](../../script/ao/bazel-remote.json).
CI starts and stops it under governed Python: the pinned Nix shell on Linux,
the selected Homebrew Python on macOS, or the interpreter selected by
`toolchain.json` on Windows. Each cache-enabled step selects the loopback URL
explicitly. Startup rejects occupied fixed ports and verifies that its own
child is live, even if HTTP status responds. Shutdown uses a bounded drain,
matching acknowledged writes to backend completions by object kind and hash.
Timeout or stalled progress is not a successful drain. The stop receipt can record prefix, traffic, failures,
and unfinished uploads without credentials. Expired blobs cause bazel-remote's
gRPC dependency check to miss; lifecycle rules govern retention, not correctness.

The proxy does not support native Windows builds. The Windows job starts the
verified Linux binary in a throwaway WSL2 distribution imported from the
pinned rootfs. WSL forwards its listeners, so Windows Bazel uses
`grpc://127.0.0.1:9092`. The rootfs has no trust store: the runner's Git
for Windows CA bundle supplies `SSL_CERT_FILE`. Only the three backend
variables named in `WSLENV` enter the distribution, and the call carrying them
does not echo shell output. Stop reads the distribution's access log, drains,
signals the process, and unregisters the distribution. If WSL2 import or
startup on the hosted `windows-2025-vs2026` image fails, an automatic run keeps
its local cache and a requested live validation fails.
