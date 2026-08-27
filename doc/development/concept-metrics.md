---
id: development.concept-metrics
type: development
status: current
domain: development
summary: Defines how contributors measure public concepts, construction hops, and include fan-out for RFC 0002.
---
# Concept metrics

## Scope

This guide owns the contributor procedure for the concept baseline introduced by
[RFC 0002](../rfc/0002-application-concept-debloat.md).
It does not own layer maps, naming policy, or product behavior.

The report answers four primary questions from one configured debug build:

1. How many public declarations exist under `include/ao` and `app/include/ao`?
2. How many directed edges join those declarations through callable ownership, public bases, fields, aliases, parameters, or return types?
3. How many API boundaries a named frontend leaf crosses before it is usable?
4. How heavy each public header is to include, and how many translation units Ninja invalidates when it changes?

Header counts, role suffixes, and `*Context` / `*Dependencies` field counts are secondary. A change that improves a secondary number while raising a primary one is rejected.

## Policy

Run the measurement through the existing dependency-report path:

```bash
./ao deps report --concepts
ao.bat deps report --concepts
```

The command writes `concept-report.json` into the selected build tree. Pass
`--json <path>` to copy the same document elsewhere. `-p` selects the build
directory; `-j` sets parallel header parses.

The denominator is frozen in `script/ao/core/concept_scope.py`:

- Core public headers: `include/ao`.
- Application public headers: `app/include/ao`.
- Each public callable overload, whether a free function or an explicit public member, is one declaration.
- Implicit declarations, anonymous namespaces, enumerators, fields, and non-public members are excluded.
- Dependency edges are type edges from the Clang AST. Callable edges retain the
  overload signature of their source declaration, and each public member
  callable has an edge to its owning type. That owner edge makes a method and
  an equivalent free function taking the owner symmetric. Header includes are
  a separate list and never stand in for those edges.
- Construction hops are the frozen frontend leaf chains in that module. A phase that touches a leaf updates the named chain in the same change.

Clang walks each public header as a self-contained translation unit using the
union of compile flags from `compile_commands.json`. Include weight is the set
of project headers that appear in that AST location graph, plus their bytes.
Rebuild fan-out is the number of Ninja translation units that list the header.
The report retains one `headerIncludes` and one `headerFanOut` row per public
header; the printed percentiles are summaries, not the only evidence.

Do not substitute regex inventory for these four measurements.

## Workflow

1. Configure and build the native debug tree so `compile_commands.json` and Ninja dependency records exist. Baseline and result must build the same complete target set before measurement; a configured but unbuilt target has no Ninja dependency records and makes fan-out incomparable.
2. Run `./ao deps report --concepts` (or `ao.bat` on Windows).
3. Read the printed summary and keep `concept-report.json` beside the build. It is a build artifact, not a source file.
4. After a concept-debloat change, build the same complete target set, run the same command on the same preset, and compare primary totals, named construction chains, and include/fan-out percentiles. If the public-header set changed, calculate both versions' percentiles over the shared-header cohort and list added and removed headers separately. This prevents deleting a low-fan-out header from masquerading as a regression by moving the median's denominator.
5. If a primary metric moves in the wrong direction, stop and correct the measurement or the change before the next phase.

Windows resolves `clang` from the pinned LLVM SDK used by tidy. Linux and macOS use `clang` from the portal environment.

## Validation

Fixture tests live in `test/script/test_concept_report.py` and run with
`./ao test --tooling` on Linux and `ao.bat test --tooling` on Windows.
They lock the scope roots, overload treatment, hop arithmetic, AST exclusions,
callable symmetry, unique-header fan-out, and the rule that concatenating
headers does not reduce the declaration count.

A live Clang dump is exercised when `clang` is on `PATH`. Completing a
concept-debloat phase still requires the full gate in
[validation and review](test/validation-and-review.md): `./ao check`, then
`./ao hygiene`.

## Troubleshooting

- `compile_commands.json not found`: configure the debug tree with `./ao build`.
- Parse failures for public headers: the report fails closed and lists the first errors. Missing include flags usually mean the compile database is stale.
- Fan-out percentiles at zero: the tree is configured but Ninja has no dependency records yet; build before comparing fan-out.
- Missing construction-chain path: the catalog in `concept_scope.py` still names a file that moved. Update the chain with the consolidation that touched it.

## Related documents

- [RFC 0002: Application concept debloat](../rfc/0002-application-concept-debloat.md) owns the proposal, protected structure, and phase order.
- [Dependency governance](dependency-governance.md) owns `./ao deps verify` and governed package identity.
- [Naming convention](naming-convention.md) owns role vocabulary; this report only counts it.
- [Application-layer review](application-layer-review.md) owns the contributor review workflow that later consumes the RFC's three questions.
