---
id: development.test.performance
type: development
status: current
domain: development
summary: Defines the reproducible performance-review workflow and report contract.
---
# Performance review

## Scope

This guide owns the contributor workflow for Aobus performance evidence.
It covers the standalone optimized workload, sampling controls, structured report, and cross-platform comparison.

Performance review is evidence for a design decision, not a machine-independent correctness test.
The workload is excluded from the default build and is not executed by `./ao check`; the check gate explicitly compiles it so source and dependency drift cannot leave the review target broken.
Ordinary behavior remains owned by the normal test suites.

## Prerequisites

Run the portal from the repository root.
Linux re-enters the governed Nix environment automatically.
Native Windows uses the managed MSVC and vcpkg environment described in [Windows development](../windows.md).

Use the same machine state, build flavor, dataset, sample count, and revision conditions when comparing two implementations.
Close unrelated load where practical, but do not present one workstation's timings as universal product thresholds.

## Workflow

Run the fixed review workload in the normal Release/IPO build:

```bash
./ao perf
```

On native Windows, use the same command vocabulary:

```bat
ao.bat perf
```

The default run performs one unmeasured warm-up and twenty measured samples per workload, then reports the median and 95th percentile.
It builds only the `EXCLUDE_FROM_ALL` `ao_perf_baseline` target and selects the `[perf][review]` Catch2 contract.

Useful controls are:

```bash
./ao perf --samples 40 --warmups 2
./ao perf --output /tmp/aobus-performance.json
./ao perf --no-build
./ao perf --filter "[perf][review]"
```

An ordering review may append a workload from an existing Aobus library:

```bash
./ao perf --library-root ~/Music --library-locale en-US
```

This optional workload opens the existing library through Aobus's normal admission path, copies only title and artist text into benchmark-owned memory, and repeats those rows to 50,000 tracks.
It does not open or copy audio files, invoke library mutation commands, or emit source text into the report; the report contains only the fixed `library-real` label and aggregate measurements.
The fixed synthetic datasets remain the authoritative cross-platform comparator, while a real library is supplemental evidence about representative text distribution.

The ordering full-rebuild scenario is projection-shaped rather than a facade-only key loop.
It models Artist grouping followed by Title ordering and applies the same dictionary-cache lookups, identity/order materialization, `StringArena` interning, and comparator sort to the current and candidate policies.
Candidate measurements include any separately derived Unicode group-identity key.
Identity materialization follows production's direct ASCII-fold fast path and uses Unicode default folding only for non-ASCII text.
The completion-vocabulary baseline retains the production raw-byte tie so projection-specific ASCII folding does not inflate it.

The completion-alias review uses the production `CompletionService` snapshot and `TrackFilterCompleter` rather than a facade-only transliteration loop.
It compares an optional-policy-disabled snapshot with the ICU policy over 50,000 ASCII tracks and a 5,155-track CJK-heavy fixture containing 1,619 distinct Han values plus repeated Kana and mixed-script values.
The measured materialization includes the track-store scan, snapshot alias memoization, aggregate merge, and retained alias bytes; fixture construction is outside the timed region.
Cached lookup rows separately cover enough whole-value hits to fill the limit, enough interior-word hits to fill it, no direct hits followed by alias hits, and a complete direct-plus-alias miss.

`--no-build` uses the selected existing flavor tree and fails when its benchmark executable is absent.
The configured compiler, sanitizer flags, and CMake build type must match the selected options; a mismatched tree is rejected before running or removing an existing report.
Its report and terminal summary mark the source revision as `unverified`: the current checkout does not establish which source produced an existing executable.
Rerun without `--no-build` when the review needs source-attributed evidence.
`-p` selects one exact build tree in the same way as other portal commands.
The portal removes the selected report before launching the workload and rejects a successful test selection that does not recreate it, so a filtered run cannot present stale evidence.
Debug, sanitizer, and profile runs may help diagnosis, but acceptance evidence uses `release` unless the reviewed question explicitly concerns another build mode.

## Report contract

The JSON report records:

- source revision and whether the worktree is dirty after a portal build, or `unverified` for `--no-build`;
- compiler, build mode, platform, and governed ICU version;
- warm-up and measured-sample counts; and
- capability, scenario, dataset, input cardinality, median, and p95 for every measurement;
- policy and locale only when those dimensions affect the measured capability; and
- an optional named byte metric when the workload produces meaningful retained or generated bytes.

The current schema identifier is `aobus-performance-review/v2`.
Ordering rows use the optional byte metric for distinct generated keys rather than every attempted generation.
Completion-alias rows omit locale because alias derivation is independent of the presentation locale, and they report snapshot alias bytes only for scenarios that materialize them.

The sampler sorts measured durations, uses the upper middle observation for the
median when the sample count is even, and uses the nearest-rank 95th percentile.
These are sample summaries, not confidence bounds or worst-case latency.

Keep before/after reports outside the repository, normally under `/tmp` on Linux or the local temporary directory on Windows.
Review both absolute latency and the relative delta.
Binary or dependency size may be reported alongside the timings, but it does not override correctness or an observed latency regression.

When a performance result justifies a code or architecture choice, summarize the stable workload and both platform results in the owning RFC or review record.
Do not check machine-specific raw reports into the repository.

Proposal-specific thresholds remain owned by their in-review RFC.
Before that RFC is deleted, an accepted long-term upgrade gate moves into one scoped section of this guide; its decision record links here and retains rationale rather than duplicating the threshold table.

## Design-audit workloads

The opt-in audit filters use the same report format and honor `--samples` and
`--warmups`. Run each filter separately with its own output path: each test case
writes one complete report. A second reporting case fails without overwriting the
first report; the partial file from that failed run is not acceptance evidence.
The portal creates missing output parent directories. Direct executable runs must
provide an existing parent directory and a fresh output path.
For example, on Linux:

```bash
AOBUS_AUDIT_OBSERVATIONS=1000 ./ao perf --filter '[audit-observation]' --samples 40 --warmups 2 --output /tmp/observations.json
AOBUS_AUDIT_COMPLETION_HISTORY=200000 ./ao perf --filter '[completion-vocabulary]' --samples 40 --warmups 2 --output /tmp/completion-history.json
AOBUS_AUDIT_QUERY_ATOMS=512 ./ao perf --filter '[audit-query]' --samples 40 --warmups 2 --output /tmp/query-boundary.json
```

On Windows, set the corresponding environment variable before `ao.bat perf`
and write reports to the guest's local temporary directory.

- The observation fixture withholds Player's owner executor while a producer
  emits graphs with 32 nodes and 128-byte ordinary node names. It measures
  producer enqueue time and total owner drain time separately, asserts one
  pending delivery and one coalesced quality notification, and checks the final
  graph. Older pre-coalescing reports retained one task and notification per
  observation; their batch drain and memory costs remain the comparison baseline. Repeat at 100,
  1,000, and 10,000 observations. The byte metric is a payload lower bound;
  allocator overhead, task wrappers, connections, and additional graph copies
  require a separate heap profile. The pending-payload byte metric now counts
  one retained graph regardless of burst length. Drain p95 describes complete batches, not
  individual callback latency. This fixture does not directly withhold Engine's
  non-realtime event worker or exercise ordered terminal events. Deterministic
  Player regressions separately withhold that worker, delay outward publication,
  race subscription retirement with delivery, and interleave a playback failure.
- The completion fixture fixes 50,000 live tracks and 93,674 aggregate values,
  while the history variable adds unused append-only dictionary entries.
  Compare zero, 200,000, and 1,000,000 retired values. Cold timing starts after
  invalidation and includes the production snapshot rebuild and aggregate
  materialization; fixture creation and invalidation delivery are outside it.
  Warm-up rebuilds also warm process-wide ICU state, so this is an invalidated
  snapshot measurement, not first-process startup. Other vocabulary rows
  measure subsequent materialization. Cached lookup samples each time one
  request, rather than averaging a batch of requests before computing p95.
  These rows are separate from the ordering-key completion vocabulary gate below.
- The query fixture covers adjacent atoms, binary expressions, parentheses,
  quoted text, and scalar lists. The input parameter counts atoms, nesting
  levels, quoted payload bytes, or list elements according to the shape; the
  report also records actual input bytes. Preparation is outside timing.
  Parsing includes the initial normalization. Admitted inputs additionally time
  renormalization, query and format compilation including temporary-plan
  destruction, serialization, and AST retirement. Rejected inputs omit phases
  that did not execute. Total duration includes timing bookkeeping. Probe both
  sides of structural and byte limits, as well as much larger rejected inputs;
  parser correctness tests remain the authority for expected admission.

A fast workstation establishes an observed result on that workstation only.
Record CPU, compiler, build settings, source identity, competing load, and sample
counts, and run timing workloads serially without concurrent builds or profilers.
For a dirty tree, preserve its diff and hashes of untracked source files outside
the repository alongside the report. Profile allocation separately: whole-process
peak heap can include fixture construction, while allocation-call-path attribution
can include long-lived caches and does not by itself identify temporary bytes.

Before accepting a change, state an interaction budget and retain headroom for
slower hardware. Explicit 3x and 5x latency sensitivity calculations can expose
risk but are hypothetical projections, not measured low-end CPU results.
A VM sharing the workstation CPU adds native-platform evidence, not independent
weak-hardware evidence. For the September 2026 audit, use 100 ms as a cold owner
request investigation threshold and 20 ms on the reference workstation as a
warning under the 5x assumption. These are review thresholds, not timer assertions
or a promised supported-library-size limit. Unbounded memory growth requires a
retention decision regardless of how quickly a finite batch drains.

## Locale-aware ordering gate

[Decision 0013](../../decision/0013-adopt-icu-collation.md) adopts the ordering
workload in this guide as the review gate for an ICU upgrade or a material
change to text-key derivation.

Acceptance evidence uses Release/IPO, one unmeasured warm-up, and twenty
measured samples on Linux and native Windows. The authoritative synthetic
matrix contains 10,000 and 50,000 rows for ASCII, Latin-diacritic, and mixed-CJK
text. The optional repeated-library workload is supplemental and must not
replace that matrix.

The full rebuild models Artist grouping followed by Title ordering. It includes
dictionary-cache lookup, locale-independent group identity, article-adjusted
group order, locale order for inline titles, `StringArena` interning and
deduplication, and final comparator sorting. The byte baseline uses the
production caller-owned scratch/arena path. For continuity with the acceptance
review, the ICU measurement also includes the independently required Unicode
identity fold while the byte baseline retains ASCII identity; this deliberately
overstates the cost attributable to collation and must remain the comparison
convention.

Future reviews must satisfy every row on both platforms:

| Workload | Median budget | p95 budget | Relative or key-byte budget |
|---|---:|---:|---:|
| 50k full rebuild | 50 ms | 75 ms | At most 3.0x the same-run byte median |
| One-row update over 50k | 5 ms | 10 ms | Absolute budget only |
| Completion vocabulary | 2 ms | 5 ms | Absolute budget only |
| Warm collator construction | 1 ms | 2 ms | One construction per startup locale |
| Unique generated key bytes | N/A | N/A | At most 1.5x the matching byte-key fixture |

Arena-backed workloads derive the byte metric from new `StringArena` interns;
vocabulary workloads deduplicate equal binary keys before summing them. It must
not be replaced with the sum of every generated key attempt. The timing budgets
are review criteria over the structured report, not Catch2 assertions; a result
outside a budget requires investigation and an explicit review decision rather
than a flaky test threshold.

## Completion-transliteration gate

The completion-alias acceptance review used Release/IPO, one warm-up, and twenty measured samples with ICU 78.3.
The synthetic CJK fixture has 5,155 tracks, 1,619 distinct Han values, repeated Kana and mixed-script values, and 24,027 retained alias bytes.
The accepted evidence was:

| Snapshot workload | Linux GCC median / p95 | Windows MSVC median / p95 |
|---|---:|---:|
| 50k ASCII, policy disabled | 14.530 / 15.555 ms | 30.274 / 32.033 ms |
| 50k ASCII, ICU policy | 15.157 / 15.679 ms | 31.743 / 34.109 ms |
| 5,155 CJK, policy disabled | 0.857 / 0.904 ms | 7.069 / 7.671 ms |
| 5,155 CJK, ICU policy | 42.622 / 43.171 ms | 59.843 / 65.605 ms |

| Cached CJK lookup | Linux GCC median / p95 | Windows MSVC median / p95 |
|---|---:|---:|
| Whole-value tier fills the limit | 0.081 / 0.083 ms | 0.109 / 0.138 ms |
| Interior-word tier fills the limit | 0.093 / 0.094 ms | 0.117 / 0.145 ms |
| No direct hit, alias tier hits | 0.096 / 0.101 ms | 0.080 / 0.106 ms |
| Complete direct-plus-alias miss | 0.121 / 0.122 ms | 0.088 / 0.116 ms |

A separate one-sample process-cold diagnostic with no warm-up measured first Kana/Han use at 4.394/53.175 ms on Linux and 6.083/45.841 ms on Windows.
Those one-shot values explain the lazy-transform design but are not percentile evidence.
Policy construction itself performs no transform lookup, so an ASCII-only application startup does not pay that cost.

An ICU upgrade or material alias-derivation change reruns the same matrix on both hosts.
Investigate rather than silently accept a 50k ASCII median above 1.10x its same-run disabled policy, a CJK snapshot p95 above 100 ms, a cached lookup p95 above 1 ms, or either process-cold transform above 100 ms.
These are review budgets over optimized builds, not test assertions.
The accepted synchronous result does not justify a background worker, persisted alias index, or startup-time eager derivation; such complexity requires new evidence.

## Validation

Changes to the portal command require:

```bash
./ao test --tooling
```

Changes to benchmark C++ or measured production code follow the normal completion gate:

```bash
./ao check
```

That gate compiles `ao_perf_baseline` but does not execute its sampled workloads.

Cross-platform claims require the equivalent native Windows run.
The benchmark itself must assert only fixture integrity and observable semantic invariants; elapsed-time regressions remain review decisions rather than flaky Catch2 thresholds.

## Troubleshooting

If the executable is missing after `--no-build`, rerun without that flag so the portal configures and builds the selected tree.
If results vary widely, increase the measured sample count and check for unrelated machine load before changing the workload.
Do not remove outliers manually; the report's median and p95 are calculated from the complete measured sample set.

## Implementation map

- [`script/ao/command/perf.py`](../../../script/ao/command/perf.py) owns portal arguments, build/run selection, metadata injection, and summary output.
- [`test/perf/`](../../../test/perf) contains the standalone Catch2 workloads.
- [`test/script/test_cli.py`](../../../test/script/test_cli.py) and [`test/script/test_buildenv.py`](../../../test/script/test_buildenv.py) protect command registration and native-environment classification.

## Related documents

- [Optimized builds](../optimized-builds.md) owns Release/IPO and profiling flavor roles.
- [Test suites](test-suite.md) owns the suites included in `./ao test` and `./ao check`.
- [Validation and review](validation-and-review.md) owns the normal completion gate.
