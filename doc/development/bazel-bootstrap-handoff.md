# Native Bazel repository bootstrap handoff

Checkpoint recorded on **2026-11-05** for the next maintainer of PR151.
This is a dated work handoff, not a second dependency policy or a claim of
three-platform acceptance. Current usage and contracts belong in the
[Bazel development guide](bazel.md#direct-bazel-without-the-portal) and
[dependency governance](dependency-governance.md).

## Resume from this checkpoint

- Branch: `feat/bazel-development`.
- Local implementation commit: `64d199aee62b601cca2ccdbb67644bc267b74432`
  (`build(bazel): bootstrap governed native SDK repositories`).
- Parent and last observed published head:
  `9e804e0e0d082806dbba5cad782d133b7581e206`.
- [PR151](https://github.com/czrocklee/Aobus/pull/151) was OPEN and draft at the
  pre-commit observation. No push, PR update or new CI dispatch accompanied this
  checkpoint. The implementation commit is one additive commit after the two
  existing review units; neither published commit was rewritten.
- The implementation commit passed normal hooks with real timestamps. This
  handoff was written afterward and is not included in that commit. Check
  `git status` before staging or publishing subsequent work.

**Next critical boundary: exact-head hosted CI has not run for the current
candidate.** Bounded native Linux, macOS x86_64 and Windows x64 source checks
are recorded below; they do not establish complete hosted-matrix acceptance.
Do not mark the PR ready or merge it on the strength of older runs.

## Resumption on 2026-11-05

The branch, local implementation HEAD and remote tip still match the checkpoint
above. PR151 remains OPEN and draft. No publication or workflow dispatch has been
performed during this resumption.

The two original `/tmp` evidence directories below are absent on this host. The
persistent native output and tooling/hygiene trees still exist. Newly recovered
GitHub evidence and local validation are retained under
`/tmp/aobus-pr151-resume-20261105/`; start with
`editor-windows-hygiene-diagnosis.md`. Both downloaded Windows artifact archives
matched their API SHA-256 digests. Historical local checks below remain recorded
checkpoint observations, not newly recovered raw receipts.

### Windows timeout diagnosis

Both predecessor Windows/Bazel jobs have now settled. Their check-run annotations
explicitly report the three-hour job limit, including the companion PR job whose
cancellation cause was previously uncertain. Recovered build logs show continued
owner-wide WinUI lint progress, not a demonstrated stuck action:

- Manual hygiene ran for 100m45s; its first report pass reached 1369/1393 actions.
- PR hygiene ran for 114m58s; its first report pass reached 1356/1393 actions.
- Both had four actions running and rotating files at interruption. Each BEP
  records `INTERRUPTED`, native exit 8, with 29 of 30 lint owners complete; WinUI
  remained incomplete. Neither hygiene gate passed.
- Format, audits, Ruff and mypy had already completed. The first report pass
  expanded selected files into 30 owners and 657 lint actions. Console silence
  reflects output redirection to `build.log`, not evidence of a hang.

No exact remaining duration, second-pass workload or parsing/checker/I/O cost
breakdown is established. Neither archive contains final gate receipts, action
report bodies or profiles. A larger budget still needs a complete native run.

### Uncommitted acceptance corrections and publication blocker

The current local CI changes retain every gate and add the real TUI beside the
Release canary in all three direct-entry steps. The canary alone has no provider
dependencies; the Release TUI exercises target-mode SDK aliases and catalog
consumption. Debug TUI execution and native compile-contract verification retain
the separate optimized catalog execution-tool boundary. This is bounded TUI
coverage, not Release GTK/AppKit/WinUI parity or Release runtime testing.

Only `windows_bazel` has its budget raised from 180 to 300 minutes, allowing
residual lint work, the additional Release build and runner variability. This is
an unvalidated operational allowance, not a completion estimate; the independent
CMake Windows job budget is unchanged.

Fresh local tooling passes: 2616 tests, 22 skipped, Ruff clean and mypy clean on
73 production files. Scoped hygiene passes for `test/script/test_bazel_ci.py`.
The Release correction received an independent source review. Documentation
validation passes for 204 Markdown files. No native execution or exact-head
hosted acceptance is newly established.

**Publication is blocked by the repository's commit-time hook.** At 15:43 +0900,
`git commit` returned zero but the hook undid the commit because automated commits
are forbidden during 13:00–21:00. HEAD remains `64d199aee`; the CI workflow and
regression-test changes remain staged. The handoff and navigation link remain
uncommitted. No timestamp override or hook bypass was attempted. Resume with a
manual commit or explicit authorization for a per-operation hook bypass; do not
treat the nominal command exit as a surviving commit. Evidence includes
`commit-ci.log`, `tooling-final.log/.exit` and `hygiene-ci.log/.exit` under the new
evidence root.

## Starlark import refactor on 2026-11-05

The first implementation slice moved native, test and catalog library
imports into checked-in Starlark. Python returns validated interface observations;
it no longer writes temporary BUILD/manifest/link-plan trees for the resolver to
reread. `bazel/import_repository.bzl` owns sparse materialization and data-only
SDK projections; `bazel/imports.bzl` owns declarations and configured aliases,
including WinUI's root aliases. The published diagnostic manifest retains its
`targets` field, now derived by Starlark rather than Python.

At this checkpoint, provider discovery, native admission checks, compiler policy
and aggregate SDK identity were unchanged. Native probes and tool/toolchain
BUILD generation still lived with the launcher; the functional follow-up below
supersedes that layout. The results in this section belong to the first-slice
source, not the subsequent ownership migration. No `rules_python` integration
or new pin was introduced. The module lock changed only its transitive Starlark
digests.

Evidence: `/tmp/aobus-native-imports-20261105/`.

- Linux tooling passes: 2631 tests, 22 skipped; Ruff and mypy clean on 73
  production files. Scoped Python hygiene passes.
- Linux default Bazel graph: 16/16 targets pass, with 14 executed and two cached.
  Debug/Release TUI help, both compile-contract gates, GTK/backend builds,
  headless GTK tests and lint fixtures pass. Bare Debug/Release TUI help passes
  with portal selectors cleared; that caller used the pinned Nix shell.
- A controlled real-Bazel fixture links ordered static archives, preserves owned
  nested-header topology, selects optimized execution tools beside Debug
  targets, rejects fastbuild and an unowned provider header, and rebuilds after
  owned-header content changes. Its final native manifest preserves target names.
  This is mechanism evidence, not cross-host or hermetic SDK acceptance.
- Native macOS x86_64 Debug/Release TUI help, compile-contract verification,
  AppKit cquery and four focused CLI/TUI/utility/catalog test targets pass in each
  mode. A separate bare AppKit cquery passes. Source bytes were compared directly
  between Linux and the shared guest checkout; HEAD equality alone was not used
  to identify these uncommitted inputs. The final diagnostic-only manifest
  correction passed another native cquery; unchanged build/test results are reused.
- macOS used an existing ready vcpkg SDK and new guest-local APFS trees; no
  provisioning, GUI launch, physical audio or ARM64 validation occurred.
- Native Windows x64 Debug/Release TUI help, compile-contract verification,
  WinUI cquery and four fresh CLI/TUI/utility/catalog test targets pass in each
  mode. Each mode reports 1211 cases passed and one existing console-dependent
  TUI skip. The verify receipts cover 1081 compile contexts, including three
  execution-tool contexts. This is not a WinUI build or GUI claim.
- Corrected Windows tooling passes: 2667 tests, 193 skipped; Ruff and mypy clean
  on 73 production files. Six-path Python hygiene passes. A cleared-selector
  bare Debug TUI/WinUI cquery passes without a portal request, not bare runtime.
  Source-byte hashes, separate local NTFS output roots and actual SDK copy
  fallback bytes were verified. Existing governed SDKs were reused unchanged.
- Independent implementation review found no concrete blocker. Mechanism unit
  tests do not substitute for the native probes above.

The user authorized 16 jobs for subsequent supported commands and existing VM
operations. macOS was started normally. A normal Windows managed-save restore
failed with native exit 1 for missing `rdseed`. Read-only host investigation
found a BIOS/microcode transition with the same kernel/initrd, plus an earlier
independent virtiofs saved-state error; retain the diagnosis in
`windows-restore/nixos-config-diagnosis.md` rather than treating either as a
Bazel failure.

The user then explicitly authorized discarding the old saved memory session.
`virsh start win2k25 --force-boot` succeeded: the VM is running with 32 GiB,
managed-save absent, SSH available and `rdseed` disabled in the normalized live
CPU XML. Disk images were retained, not removed or replaced; BIOS, host safety
settings and persistent VM configuration were unchanged. Windows gates above
ran against verified shared-source bytes.
The first tooling attempt exposed Git's trust check for the read-only `Y:/`
share and two tests leaking into native discovery. That failed attempt is
retained under `windows/`, not counted as passing acceptance. The corrections
pin the environment test's fixture host and isolate the missing-cache test from
the real managed cache; validators and assertions were not relaxed. Git trust
was scoped to the verified `Y:/` path in one disposable process environment,
without global configuration or a wildcard exception. See `windows/summary.md`
for native exits, fresh test BEPs, copied logs and declared evidence limits.

All implementation and documentation changes remain uncommitted; the
publication hook blocker above has not been bypassed. No push or CI dispatch
has occurred.

## Functional ownership follow-up

The next uncommitted slice follows the functional owners documented in
[Architecture](bazel.md#architecture). `bazel_workspace.py` now owns managed
Bazel and portal launch/output/log behavior, not provider, library, host-tool or
compiler discovery. Repository probes no longer import the launcher or mutate
its private globals. Scoped probe observation restores the previous observer
on normal, exceptional and nested exits.

Host-tool and compiler observations now cross the resolver boundary as data.
Checked-in Starlark owns their sparse artifacts, metadata values and fixed
BUILD entrypoints; compiler policy still uses the existing three native macros.
The former generated toolchain directory/readback is gone. WinUI's independent
lazy SDK projection remains with its existing owner. No dependency pin, compiler
policy, SDK identity input family or Python runtime distribution was replaced;
compact identity serialization may cause a one-time action-cache miss.

Evidence: `/tmp/aobus-functional-owners-20261105/`.

- A normalized AST comparison confirms 46 moved native/provider/library
  admission bodies are unchanged apart from their owner qualification. Scoped
  observation and tool/compiler projection are separately tested contracts.
- Full Linux tooling passes: 2648 tests, 22 skipped; Ruff clean and mypy clean on
  78 production files. The initial lint/type failures are retained and corrected,
  not counted as a passing gate.
- Fresh Linux Debug TUI help, compile-contract verification and all 16 default
  test targets pass; all tests executed with test-result caching disabled.
- Linux Release TUI help, verify and four fresh targets pass. GTK/backend build,
  a fresh headless GTK test and the native lint fixture suite pass. Bare Debug
  and Release TUI help execute successfully with portal selectors cleared;
  the caller used the pinned Nix shell. See `linux/summary.md` for cache limits.
- Native macOS x86_64 Debug/Release TUI help, verify, four fresh targets per
  mode, AppKit cquery and bare Debug AppKit/TUI cquery pass. Each mode has 1212
  Catch2 cases and 55080 assertions, no skipped cases. The first bare command's
  invalid multiple-label syntax exited 2 and is retained; the corrected
  `set(...)` query exits 0. Final 19-file Python hygiene passes.
- Native Windows x64 tooling passes after correcting a test fake's native-path
  rendering to model Bazel Path: 2684 tests, 193 skipped, Ruff/mypy clean on 78
  files. The initial failed log is retained; exact source/destination/watch and
  negative assertions remain. Git trust is still exact `Y:/`, process-scoped.
- Windows Debug/Release actual TUI help, both verify gates, four fresh targets
  per mode and WinUI cquery pass. Each mode has 1211 cases passed and one existing
  console-dependent skip; verify covers 1081 compile contexts, including three
  optimized execution-tool contexts. Cleared-selector bare TUI/WinUI cquery and
  48-path Python hygiene pass. These queries are not GUI/runtime acceptance.
- All native runs use existing installed SDKs unchanged and compare host/guest
  source bytes; no provisioning occurred. Actual fetched tools/compiler graphs
  use the checked-in templates, and read inventories exclude the launcher.
  The ordinary generated lock refresh changes only three transitive Starlark
  digests, not pins or recorded input maps. Independent source review found no
  concrete admission, identity or graph blocker. See `macos/SUMMARY.md` and
  `windows/summary.md`; ARM64, GUI/audio, full CMake and exact-head hosted/R2
  acceptance remain unvalidated.

## Starlark compiler construction follow-up

The subsequent slice removes the remaining construction decisions from the
compiler probe. `bazel_compiler_probe.py` replaces `bazel_toolchain.py` without
a compatibility shim and returns only platform, raw SDK observations and the
existing input identity. Starlark owns sparse tool labels/destinations, basename
collisions, native-machine-to-Bazel CPU mapping and Linux mode-specific salted
hardening environments. The probe retains native SDK admission and the broad
SDK identity input inventory; the macOS/Windows compiler backends are not rewritten.

The compile-contract checker keeps an independent expectation oracle over the
raw Nix report. That checker is not a toolchain producer, so a bad generated mode
environment cannot pass merely by sharing its generation helper.

Evidence: `/tmp/aobus-starlark-toolchain-20261105/`.

- Final Linux tooling passes: 2668 tests, 22 skipped; Ruff and mypy clean on 78
  production files. Existing rejection assertions and prior platform skips remain.
- The first real Bazel load rejected a Python-valid `\\x7f` string escape and
  exited 37 during selection. Its failure receipt and stderr are retained.
  The equivalent Starlark octal spelling fixes the issue; the corrected native
  Debug compile-contract gate exits 0. Python-subset mechanism tests alone are
  not accepted as Starlark load evidence.
- Linux native Debug TUI help, all 16 fresh default test targets, Release TUI
  help/verify/four fresh targets, GTK/backend build, fresh headless GTK and lint
  fixtures pass. Bare Debug/Release TUI help passes. Actual target-dbg excludes
  fortify while exec-opt and target-opt retain the full observed Nix policy.
  A separate real-Bazel fixture accepts a legal Unicode tool basename and
  analyzes the projected graph; it does not compile a new compiler SDK.
- macOS x86_64 actual Debug/Release TUI help, verify, four fresh targets per mode,
  AppKit cquery and cleared-selector bare TUI/AppKit cquery pass. Each mode has
  1212 Catch2 cases and 55080 assertions. Within each reused tree, compiler
  BUILD/native-path bytes and all 17916 SDK identity entries match the retained
  pre-migration snapshot. Final scoped Python hygiene passes.
- Windows x64 corrected tooling passes: 2704 tests, 193 skipped; Ruff and mypy
  clean on 78 files. The first run exposed native Windows separators in two
  fake Linux SDK fixtures. Their raw report paths now use target-appropriate
  forward slashes, with separate native ledger expectations; exact lexical,
  destination, identity/hash and rejection assertions remain. Initial failure
  evidence is retained, not counted as acceptance.
- Windows actual Debug/Release TUI help, verify, four fresh targets per mode,
  WinUI cquery and cleared-selector bare TUI/WinUI cquery pass. Each mode has
  1211 cases passed and one unchanged console-dependent skip. Verify retains
  1081 compile contexts, including three optimized execution-tool contexts.
  Compiler metadata and all 5889 SDK identity entries match pre-migration;
  final 13-path Python hygiene passes. C++ cache reuse is declared, not hidden.
- Independent review confirms the observation/generation/checker separation and
  preserved SDK guards/input families. Host/guest source hashes match, SDKs are
  reused unchanged and the old module is absent from source/read inventories.
  Final Linux hygiene and affected-fixture rerun pass. See `linux/summary.md`,
  `macos/SUMMARY.md` and `windows/summary.md` for native receipts and cache limits;
  GUI/audio, ARM64, full CMake and exact-head hosted/R2 boundaries remain open.

## What is implemented

The shared [`aobus_environment` repository](../../bazel/environment.bzl) prepares
one mode-neutral environment. The existing dependency, toolchain, tool and lazy
lint repositories consume that environment instead of each entering Nix or
requiring a portal-generated request. Its
[stdlib helper](../../script/ao/core/bazel_environment.py) validates the selected
host, interpreter, provider inputs and legacy request, and emits an allowlisted
environment plus watched-input inventory.

Linux enters the project's pinned `nix-shell --pure` at repository time; Nix may
realize missing pinned store outputs. Installed Nix, Bazel and a startup JVM are
still prerequisites. On macOS/Windows, discovery is ready-only: the existing
governed Python, compiler/SDK and explicitly selected vcpkg prefix must already
exist. Repository fetch does not install those platforms' Python, ports, SDKs or
WinUI runtime. Invalid explicit selections fail rather than falling back.

[`bazel_resolver.py`](../../script/ao/core/bazel_resolver.py) supplies Debug and
Release interface observations. The repository projection materializes their
subpackages, and checked-in Starlark aliases select each consumer's compilation
mode. Target and execution configurations are separate; execution tools can use
optimized interfaces beside Debug targets. The root rc
now defaults bare commands to `dbg`; `--config=release` selects `opt`, and explicit
`fastbuild` fails closed. Portal preparation is mode-neutral, while old
single-mode requests remain supported. WinUI stays lazy and may discover an
existing governed ready NuGet cache when no explicit root was selected.

Two review findings were fixed: Windows request/interpreter selectors are
case-insensitive, and all environment-helper launch paths use isolated Python
with a repository-owned `pycache_prefix`. `-B` alone does not prevent reading
stale source-backed bytecode. The Windows CI triplet query uses the same
isolation and captures the native exit before parsing stdout.

The separate observed Release lint collision is also fixed: PIC and non-PIC
compile contexts sharing a physical main no longer share a report output stem.
The report identity still retains all contexts and fails closed on hash
collisions; it does not discard an action to make analysis succeed.

No C++ runtime/test behavior, dependency pins, Python provider distribution or
`rules_python` dependency was changed. Sanitizers, coverage, full completion and
release/distribution migration remain out of scope. Host installer/registry
selection is not completely watchable; after such changes the documented
`bazel fetch --force --configure` refresh may be required.

## Validation available for the implementation

These checks ran on the worktree subsequently committed above. Direct-probe
receipts therefore identify the parent HEAD plus local changes, not a run made
after the commit. Commit creation did not change those validated inputs.

| Check | Observed result and limit |
|---|---|
| Controlled Linux direct entry | TUI cquery; Debug and Release TUI `--help`; Debug CLI `--help`; two CLI/TUI Bazel test targets; Release `//test/lint:cache_clean`: successful native commands |
| Configuration switches | Debug/Release cquery switches reused the shared environment file without changing its mtime |
| Negative controls | Missing Nix and explicit `fastbuild` failed as intended; recovery succeeded |
| Release lint regression | Focused tests passed; real portal Debug/Release canaries succeeded, with two distinct Release context reports |
| Full tooling, latest run | 2616 tests reported, 22 skipped; Ruff clean; mypy clean on 73 production files (`tooling02.log`, exit 0) |
| Win32 static typing | mypy `--platform win32` clean on 73 files; this was Linux-hosted static analysis, not Windows execution |
| Scoped hygiene | Twelve changed Python files passed; the subsequently edited CI test passed its own final hygiene rerun |
| Dependency lock | Governed `ao bazel deps` succeeded and refreshed the checked-in module lock |
| Documentation | 203 Markdown files passed before this handoff; the handoff and its navigation link also passed the 204-file documentation check |
| Independent reviews | Bounded implementation and final CI source reviews found no remaining concrete blocker; neither was native execution |

The controlled Linux caller cleared portal request/Python and incoming Nix SDK
selectors, while explicitly selecting the pinned Bazel, JVM and owned output
roots. Existing Nix store and build caches were available. These results do not
establish all-fresh compilation, offline acquisition, hermeticity, cross-host
cache portability, interactive TUI rendering or media playback.

### Local evidence to preserve

Primary evidence: `/tmp/aobus-native-repositories.s2vuhb9z/`.
Important entries:

- `PLAN.md`, `run_bazel.py`, direct-phase `*.receipt.json`, `*.log`, BEPs and
  controller exit files.
- `tooling02.log/.exit`, `mypy-win3201.log/.exit`, `hygiene01.log/.exit`,
  `hygiene-ci02.log/.exit`, `docs01.log/.exit`, `handoff-docs01.log/.exit`,
  `handoff-docs02.log/.exit`, `deps-lock01.log/.exit`.
- `mode-switch-result.json`, `negative-fastbuild01.*`,
  `negative-no-nix01.*`, `recovery01.*`.
- `lint-debug01.*`, `lint-release01.*`, `ci-lint-collision/`,
  `tests-environment-rules/`, `tests-modes/` and `environment-helper/`.
- `commit-bootstrap.log/.exit` and `pr-before-commit.json`.

Native output base:
`/home/rocklee/.cache/Aobus/build/aobus-native-repositories.s2vuhb9z`.
Tooling/hygiene tree:
`/home/rocklee/.cache/Aobus/build/Aobus-claude-fix-debug`.
Use one foreground writer per tree. The original checkpoint used eight jobs;
the user now authorizes 16 jobs for subsequent supported commands.
`ao test --tooling` has no jobs option. Preserve diagnostic trees.
These are host-local evidence locations, not portable repository artifacts.

Two ad-hoc `nix-shell -p python3Packages.pyyaml` attempts during CI editing did not
establish a governed package-set pin, and their pipelines obscured inner exit
statuses. They are **excluded from acceptance**. The record is
`ci-bare/INCIDENT.md` under the primary evidence directory. Do not assume no
external effects, repeat the installation or delete its unknown artifacts.

## CI changes and unresolved predecessor evidence

The new strict `Test direct Bazel entry` steps use separate bare output bases,
clear portal/Nix selectors, run the default Debug TUI help entrypoint, then build
a Release canary in the same base. They retain the existing product, graph,
verify, lint-fixture, hygiene and R2 gates. Windows uploads direct BEPs separately
from the portal tree to avoid a drive-root common ancestor. The combined Linux
Bazel/CMake job now has a 180-minute budget instead of 90; that budget change has
not itself been validated on a hosted runner. The implementation checkpoint kept
Windows' 180-minute budget; the uncommitted correction above changes it to 300.

The following is **older-head evidence only**, all for published `9e804e0e`, not
for the new implementation:

- PR run [41846972908](https://github.com/czrocklee/Aobus/actions/runs/41846972908)
  and manual Bazel/R2 run
  [41846998889](https://github.com/czrocklee/Aobus/actions/runs/41846998889).
- Manual Linux/Bazel, macOS Intel/Bazel and the dedicated R2 proof succeeded.
  The accepted R2 proof was same-runner Linux producer/fresh-consumer evidence,
  not cross-host portability or backend read-only authorization.
- Both Windows/Bazel tooling logs reported 2609 tests, 155 skips, governed LLVM
  and managed Python; products, graph, verify and lint fixtures succeeded.
  **Manual Windows/Bazel exceeded the confirmed three-hour job limit during
  hygiene.** Hygiene did not complete, and no native success exit is available
  for it. The resumption diagnosis above now establishes continued lint progress
  until the job budget expired.
- At the original checkpoint, the companion Windows hygiene step was canceled
  with cleanup complete, while its job and whole-run metadata still lagged.
  Recovered annotations now establish that it also hit the job limit; no native
  success or completed hygiene is implied.
- Manual ARM64 never acquired a hosted runner. Successful ordinary ARM64 jobs do
  not fill this unexecuted manual R2-bound platform coverage.
- PR Linux hit a confirmed 90-minute job timeout during the later CMake build.
  A separate Release lint step had actual exit 2 despite an optional hosted
  success conclusion; its collision fix is local evidence above, not a repair
  retroactively applied to that run.
- An earlier Windows async cancellation assertion in
  [`AsyncRuntimeTest.cpp`](../../test/runtime/AsyncRuntimeTest.cpp),
  `sleeper.tryWaitForCancellation(0)`, failed with native exit 42. Later passes
  do not establish a fix; the relevant C++ remains unchanged.

Retained predecessor evidence is under
`/tmp/aobus-pr151-bazel-ci.miyuun35/`: `ci-monitor05/report-final-139.md`,
`R2-ACCEPTANCE05.md`, and `async-diagnosis05/diagnosis.md`. Do not substitute
wrapper success, cache hits or old tooling counts for current native receipts.

## Next work, in order

1. Inspect the branch, index, remote tip and all uncommitted changes before
   publication. Preserve the staged CI correction and review coherent import,
   functional-owner, test and documentation units. Use the ordinary commit hook
   when permitted; if its time window blocks a commit, retain the changes for
   manual action or explicit per-operation authorization. Preserve real
   timestamps and existing review units; do not silently rewrite published history.
2. Publish the intended candidate and verify the remote OID. Run the new
   three-platform direct-entry gates and existing native matrix. Require both
   consumer modes and target/exec separation on native macOS and Windows;
   Linux tests and Win32 mypy are not substitutes.
3. Dispatch the existing strict manual route with `mode=bazel,r2=true` for that
   exact head, retaining real producer/fresh-consumer receipts. Reuse only the
   existing authorized backend/credentials; do not query or change secrets,
   IAM, bucket policy or repository variables. Consumer upload flags/prefixes
   do not turn writer credentials into backend read-only authorization.
4. Validate the Linux combined-job 180-minute and Windows/Bazel 300-minute
   budgets with actual hosted exits. The predecessor Windows diagnosis permits
   a budget correction, not gate skipping, relaxed assertions or retry-until-green.
   Preserve incomplete attempts if either budget is exhausted again.
5. Review complete native exits, BEPs and remote-hit evidence for that head;
   record capacity gaps, incomplete hygiene and unresolved async behavior
   explicitly. Keep the PR draft and do not merge or change `main` without
   separate authorization.

Do not reapply the old private experiment overlay, resurrect the retired mixed
Bazel lint proposal, resume canceled historical CLI/TUI matrix trees, or promote
this bounded implementation into a completed CMake/release migration. Durable
contract updates belong in the linked guides; update or retire this handoff when
its remaining acceptance work is resolved.
