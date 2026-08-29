---
id: decision.0017.evaluate-concept-metrics-as-a-vector
type: decision
status: accepted
domain: development
summary: Records why concept-debloat metrics are evaluated together rather than as independent vetoes.
---
# Decision 0017: evaluate concept metrics as a vector

## Context

The application-concept-debloat review introduced reproducible measurements for public declarations, dependency edges, construction hops, include weight, and rebuild fan-out.
The first policy treated every primary measurement as an independent veto: a change was rejected if any one value moved in the wrong direction.

Those measurements are useful evidence, but they are imperfect proxies for ownership and capability boundaries.
Moving a declaration to its real owner can reduce authority and dependency fan-out while making one median header slightly heavier.
An absolute per-number veto would reject that exchange and encourage changes optimized for the counter rather than the architecture.

The completed refactor was measured from the same complete Linux debug target set before and after the work:

| Primary measurement | Before | After | Change |
|---|---:|---:|---:|
| Application declarations | 1,718 | 1,554 | -164 |
| Application dependency edges | 2,533 | 2,305 | -228 |
| Application include weight, median | 5 headers / 10,006 B | 5 headers / 10,570 B | 0 headers / +564 B |
| Application include weight, p95 | 26 headers / 83,823 B | 21 headers / 51,298 B | -5 headers / -32,525 B |
| Application rebuild fan-out, median / p95 | 18 / 215 TUs | 17 / 142 TUs | -1 / -73 TUs |
| Core declarations | 1,558 | 1,546 | -12 |
| Core dependency edges | 1,748 | 1,742 | -6 |
| Core include weight, median | 3 headers / 7,631 B | 3 headers / 7,962 B | 0 headers / +331 B |
| Core include weight, p95 | 16 headers / 64,565 B | 16 headers / 64,565 B | unchanged |
| Core rebuild fan-out, median / p95 | 78 / 396 TUs | 80 / 342 TUs | +2 / -54 TUs |
| Named construction hops | 5, 2, 2, 2, 2 | 3, 1, 1, 1, 1 | every chain reduced |

The two median regressions are real and remain visible.
They do not outweigh the reductions in declarations, dependency edges, every named construction chain, p95 include weight, and p95 rebuild fan-out.

## Decision

Concept metrics are evaluated as one evidence vector together with the concrete ownership, capability, and behavioral result.
No individual number is an independent veto.

Every material regression must still be measured on a comparable build, explained, and corrected when it is noise or avoidable cost.
A change is retained only when its concrete gain outweighs the local regression and the overall result advances the repository.
This decision does not exempt the refactor that motivated it from publishing its own before-and-after vector.

## Alternatives considered

- **Keep an absolute veto for every primary metric.** Rejected because correlated metrics can move in opposite directions when a false abstraction is split or a capability moves to its real owner.
- **Remove quantitative concept measurements.** Rejected because declaration, edge, hop, include, and fan-out evidence exposes structural costs that review prose alone can miss.
- **Collapse the vector into one weighted score.** Rejected because fixed weights would hide the individual tradeoffs and give an arbitrary scalar more authority than the architecture it approximates.

## Consequences

Review requires explicit judgment instead of a mechanical pass/fail comparison.
Before-and-after reports must remain comparable, and any regression must be visible rather than averaged away.
Reviewers can reject a change whose claimed ownership gain is vague, whose regression is avoidable, or whose overall vector does not advance the repository.

## Current authorities

- [Concept metrics](../development/concept-metrics.md)
- [Application-layer review](../development/application-layer-review.md)
- [Validation and review](../development/test/validation-and-review.md)

## Supersession

None.
