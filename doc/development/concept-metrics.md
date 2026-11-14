---
id: development.concept-metrics
---
# Concept metrics

## Scope

This guide owns the contributor procedure for the repository's public concept
baseline. It does not own layer maps, naming policy, or product behavior.

The report answers four primary questions for one configured native build:

1. How many public declarations exist under `include/ao` and `app/include/ao`?
2. How many directed edges join those declarations through callable ownership,
   public bases, fields, aliases, parameters, or return types?
3. How many API boundaries does a named frontend leaf cross before it is usable?
4. How heavy is each public header to include, and how many selected translation
   units include it in a fresh native compiler observation?

Header counts, role suffixes, and collaborator-aggregate field counts
(`*Context`, `*Dependencies`, and `*Collaborators`) are secondary. Primary metrics
are decision evidence, not independent vetoes: a small local increase is
acceptable when the change removes a false abstraction, closes an ownership
boundary, or makes a capability exact. Record the cause and judge the complete
metric vector together with the behavioral result.
[Decision 0017](../decision/0017-evaluate-concept-metrics-as-a-vector.md) records
the rationale and the refactor evidence behind this policy.

## Workflow

Use the dependency-report command with a ready native SDK and the independently
admitted Clang analysis SDK:

```bash
./ao deps report --concepts --config debug --jobs 16
ao.bat deps report --concepts --config debug --jobs 16
```

Debug is the default; `--config release` measures the Release context. Sampling
and sanitizer profiles are not accepted by this command. `-p` selects the build
tree, and `-j` bounds native dependency passes and public-header parses. Native
macOS and Windows can select an existing SDK with `--vcpkg-prefix`; measurement
does not install or repair one.

The command verifies the current native/test SDK facts, builds generated and
compiled prerequisites, then measures them. It does **not** execute tests,
manual harnesses, or the completion gate. Its roots are `//:check_products`,
`//:check_native_tests`, `//:check_runs`, and compatible test leaves of
`//:check_tests`. Root DefaultInfo outputs and output-to-input action
reachability select the cohort: first-party target compiler actions are the
primary measurement; execution-tool actions are inventoried separately.
Unused alternative PIC actions are not additional measured translation units.

A successful command atomically writes `concept-report.json` into the selected
tree. `--json <path>` copies the same document elsewhere. A failed acquisition
does not replace an earlier canonical report; keep the attempt evidence named
by its error rather than interpreting an old report as the failed run's result.

**Migration validation boundary:** the Bazel replacement is integrated and has
focused fixture coverage. Complete native reports and cross-platform parity
have not yet been accepted. Command wiring or fixture success is not proof of
that remaining native boundary.

## Measurement contract

The denominator is frozen in `script/ao/core/concept_scope.py`:

- Core public headers: `include/ao`.
- Application public headers: `app/include/ao`.
- Each public callable overload, whether a free function or an explicit public
  member, is one declaration.
- Implicit declarations, anonymous namespaces, enumerators, fields, and
  non-public members are excluded.
- Dependency edges are type edges from the Clang AST. Callable edges retain the
  overload signature of their source declaration, and each public member
  callable has an edge to its owning type. That owner edge makes a method and
  an equivalent free function taking the owner symmetric. Header includes are
  a separate list and never stand in for those edges.
- Construction hops are the frozen frontend leaf chains in that module. A phase
  that touches a leaf updates the named chain in the same change.

Clang parses each public header as a self-contained translation unit using one
selected configured consumer context, not a union of unrelated compile flags.
Its admitted analysis compiler and arguments are recorded separately from the
native compiler used for include observations. Include weight is the set of
project headers in the AST location graph, plus their bytes.

Schema 4 names the fan-out metric `compilerIncludeFanOut`. It counts selected
first-party target compiler actions whose **fresh compiler-reported include
closure** contains the header. Native Clang preprocessing preserves diagnostics
and compile context while redirecting known outputs. MSVC preprocessing retains
textual forced includes and native diagnostics, and consumes its validated
source-dependency JSON. There is no fallback to counting Bazel mandatory/pruned
inputs, scanning directives, substituting Clang for native MSVC observations, or
filling missing evidence with zeros.

Inventory and measured passes must agree on their dependency closure. Named
input/output identities and hashes, configured roots, graph, SDK facts and
compiler identities are checked around acquisition under the selected tree's
reentrant lock. The native toolchain guard follows each platform's identity
contract: Linux guards the recorded Nix ledger digests, while macOS binds the
small schema-2 selection metadata and trusts the installed-SDK boundary shared
with Windows, without rehashing installed SDK members. Both keep the actual
observed-input and cohort guards. This is not a source-wide edit lock; do not edit measured inputs
while the command owns its foreground children.

The report retains one `headerIncludes` and one `headerFanOut` row per public
header; printed percentiles are summaries, not the only evidence. A genuine
zero include count is valid. Missing observations instead fail the report.

This is **not** Ninja dependency-log replay, the exact include history of a
previous compilation, a Bazel invalidation set, or an actual recompilation
count. Historical `rebuildFanOut` values and schema-4 observations have different
acquisition contracts; do not silently compare them as the same measurement.
Do not substitute regex inventory for these four primary measurements.

## Compare changes

1. Select the same platform, configuration, feature defaults and complete roots
   for both measurements. The command builds prerequisites itself; an earlier
   partial build is not the cohort authority.
2. Run the command and retain its JSON plus acquisition provenance beside the
   selected build. Reports are build artifacts, not source files.
3. After the change, rerun the same acquisition and compare public totals, named
   construction chains, and include/fan-out percentiles. If public-header
   membership changed, also compare percentiles over the shared-header cohort
   and list added/removed headers. Deleting a low-fan-out header must not disguise
   a denominator change as a regression.
4. Explain material regressions. Correct measurement noise; otherwise retain a
   local increase only when the concrete ownership, capability, or behavioral
   gain outweighs its cost and the complete result still advances the repository.

Successful contexts may consume their command-owned preprocessed bodies after
recording path, hash and size and passing their guards. Failed-context bodies,
streams, dependency records, identity receipts and measured raw ASTs remain
retained. Later report failure does not recreate already consumed successful
bodies. Provenance records retained raw-AST bytes; check storage headroom before
a full cohort instead of deleting evidence or narrowing it silently.

## Validation and troubleshooting

Fixture contracts live in `test/script/test_concept_report.py`,
`test/script/test_bazel_concepts.py`, and `test/script/test_deps_concepts.py`.
They retain independent declaration/overload/edge/hop arithmetic, complete
public-header membership, exact action reachability, observed-versus-declared
includes, coherent input guards, child settlement, JSON publication and public
command wiring. Isolated live AST fixtures do not establish native SDK or
complete-report acceptance.

- Missing SDK or generated inputs: follow the named admission/build failure;
  there is no compiler or layout fallback.
- Unsupported native phase, PCH, module or output behavior: preserve the failed
  context and extend its explicit contract only with native evidence.
- Changed include closure or input hash: stop editing and acquire a new coherent
  attempt; do not reuse the rejected report or call it zero fan-out.
- Public-header parse failure: inspect the retained compiler arguments and
  diagnostics for its selected context.
- Missing construction-chain path: update the catalog with the consolidation
  that moved its named source.

Completing a concept-debloat phase still requires the owning route in
[validation and review](test/validation-and-review.md), including the applicable
native `check` and check-only `hygiene`; a metric report does not replace them.

## Related documents

- [Decision 0017](../decision/0017-evaluate-concept-metrics-as-a-vector.md)
  records why no individual measurement is an independent veto.
- [Dependency governance](dependency-governance.md) owns governed package identity.
- [Naming convention](naming-convention.md#review) owns role vocabulary and
  public-role justification.
- [Application-layer review](application-layer-review.md) applies ownership review
  to runtime, UIModel, and frontends.
- [Design review](design-review.md) combines measurement evidence with behavioral
  and structural acceptance.
