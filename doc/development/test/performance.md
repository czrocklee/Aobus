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
./ao perf --filter "[perf][review][ordering]"
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

`--no-build` uses the selected existing flavor tree and fails when its benchmark executable is absent.
`-p` selects one exact build tree in the same way as other portal commands.
The portal removes the selected report before launching the workload and rejects a successful test selection that does not recreate it, so a filtered run cannot present stale evidence.
Debug, sanitizer, and profile runs may help diagnosis, but acceptance evidence uses `release` unless the reviewed question explicitly concerns another build mode.

## Report contract

The JSON report records:

- source revision and whether the worktree is dirty;
- compiler, build mode, platform, and governed ICU version;
- warm-up and measured-sample counts; and
- policy, scenario, locale, dataset, cardinality, median, p95, and `generated_key_bytes` for every measurement; the byte metric counts each distinct generated key once rather than every attempted generation.

Keep before/after reports outside the repository, normally under `/tmp` on Linux or the local temporary directory on Windows.
Review both absolute latency and the relative delta.
Binary or dependency size may be reported alongside the timings, but it does not override correctness or an observed latency regression.

When a performance result justifies a code or architecture choice, summarize the stable workload and both platform results in the owning RFC or review record.
Do not check machine-specific raw reports into the repository.

Proposal-specific thresholds remain owned by their in-review RFC.
Before that RFC is deleted, an accepted long-term upgrade gate moves into one scoped section of this guide; its decision record links here and retains rationale rather than duplicating the threshold table.

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
