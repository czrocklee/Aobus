---
id: rfc.0004.scan-file-revalidation
type: rfc
status: draft
domain: library
summary: Proposes rechecking actionable scan paths before a prepared plan mutates the library.
depends-on: none
---
# RFC 0004: Scan file revalidation

## Problem

`ScanPlan` records each file's size and modification time, and records missing paths by their absence during planning.
`ScanApplyOperation` later parses and fingerprints live files during preparation, but its final revalidation covers only `Moved` items.

`New` and `Changed` items therefore write the plan-time size and modification time even when preparation read a file that changed after planning.
A `Missing` item can also mark a manifest row missing after a regular file has reappeared at that URI.
Both cases commit a stale reconciliation that only a later scan repairs.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: None.

## Goals

- Recheck every actionable path after preparation and before database mutation.
- Skip a new or changed item when its live size or modification time no longer matches the plan.
- Skip a missing item when a regular file has reappeared at its URI.
- Preserve the existing moved-file identity check and whole-plan transaction.
- Report stale items through the existing per-item failure path.

## Non-goals

- Split a scan into batches or change whole-plan atomicity.
- Add task phases, progress kinds, retries, or new result types.
- Lock media files against concurrent filesystem changes.
- Change YAML transfer or audio-identity backfill.
- Define performance or memory budgets without measurements.

## Proposed design

Add one small file-stat helper beside `ScanApplyOperation` that resolves a plan URI and returns whether it names a regular file and, if so, its size and modification time using the planner's existing conversion.

Extend the current final revalidation pass:

- `New` and `Changed` require a regular file whose facts exactly match the plan; otherwise the item is reported and skipped.
- `Missing` is skipped when the URI now names a regular file; an inspection error is also reported and skipped.
- `Moved` keeps its existing live fingerprint comparison and also requires matching planned file facts.
- `Unchanged` and `Error` need no additional work.

An accepted new, changed, or moved item may continue writing the planned facts because the pass has just matched them to the prepared input.
An ordinary stale item does not block other valid items from committing.
The existing complete abort for a moved identity mismatch remains because committing an unsafe relink is worse than deferring the plan.

The check narrows the current plan-to-apply race; it does not claim filesystem locking or protection from a change after the final check.

## Alternatives

### Replace planned facts with whatever is live at commit time

This can pair metadata parsed from one file state with size and time from another.
Rejecting the item keeps parsed data and manifest facts tied to the same plan.

### Rebuild the complete plan on any stale item

That is correct but discards unrelated prepared work.
The existing apply path already treats ordinary item failures independently.

### Restore the former scalable-task proposal

Batching, backpressure, cancellation phases, and transfer staging do not fix this race and lack current evidence as one combined project.

## Compatibility and migration

No stored format or public API changes.
A scan may now report and skip an item that it previously committed with stale facts; a later scan can classify the new filesystem state.

## Validation

- Change a new file between planning and final revalidation and prove no track or manifest row is added for it.
- Change a changed file during the same interval and prove its previous records remain unchanged.
- Recreate a missing path and prove its manifest row remains available.
- Prove valid sibling items still commit when one item is stale.
- Keep the existing moved-destination identity mismatch and cancellation tests passing.

## Promotion plan

Update the library architecture and scan-and-identity specification with the final revalidation rule, then delete this RFC after implementation.
