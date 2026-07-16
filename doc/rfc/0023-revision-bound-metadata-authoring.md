---
id: rfc.0023.revision-bound-metadata-authoring
type: rfc
status: implemented
domain: presentation
summary: Introduced runtime-revision-bound metadata and tag authoring with stable targets, atomic outcomes, and guarded undo.
depends-on: rfc.0003.library-mutation-pipeline, rfc.0022.transaction-coherent-library-dictionary
---
# RFC 0023: Revision-bound metadata authoring

## Disposition

Implemented on 2026-07-16 on top of the coordinator from [RFC 0003](0003-library-mutation-pipeline.md).

Runtime-created `BoundTrackTargets` records one runtime instance, one committed library revision, and one exact target order.
Metadata and tag commits accept no raw-id compatibility overload: coordinator admission rechecks runtime identity, availability, revision, and every target before applying the command atomically.
Typed results distinguish `Applied`, `NoOp`, `Stale`, `Missing`, and `Unavailable`; an applied result returns the next-revision binding, while no-op preserves the original binding without committing.

UIModel `TrackAuthoringSession` owns that evidence across one editor lifetime, observes availability, and never retargets a draft after selection, projection, runtime, or revision change.
GTK inline, detail, properties, tag, and custom-metadata paths submit through the session.
Custom-metadata undo retains the applied operation's next binding, so any intervening effective commit makes the reverse patch stale instead of overwriting newer state.

The [library architecture](../architecture/library.md), [presentation architecture](../architecture/presentation.md), [mutation specification](../spec/library/runtime/mutation.md), [metadata-editing specification](../spec/presentation/metadata-editing.md), and [GTK track-detail specification](../spec/linux-gtk/track-detail.md) now own current behavior and supersede the proposal wording below.

## Problem

Aobus metadata and tag editors derive a draft from a runtime projection, then submit only target ids and new values.
The writer opens a fresh transaction and applies the patch to whatever records currently occupy those ids.

While an editor is open, scan apply, import, another editor, list/track administration, or active-library replacement may commit.
The current writer silently skips missing targets, and frontend-local undo unconditionally restores an old value.
The result cannot distinguish a no-op from disappeared targets, cannot prove that an edit still belongs to the runtime in which it began, and can overwrite newer work.

Per-field expected-value comparison could preserve more concurrency, but Aobus has one local writable runtime and exclusive maintenance already makes concurrent authoring unusual.
Carrying and validating a per-target baseline for every editable field would add a second, complex snapshot protocol without demonstrated product value.

## Dependencies

- Hard: [RFC 0003](0003-library-mutation-pipeline.md), [RFC 0022](0022-transaction-coherent-library-dictionary.md).
- Conditional: None.
- Integration: [RFC 0013](0013-coherent-application-reporting-policy.md), [RFC 0016](0016-coherent-workspace-transactions.md).

RFC 0003 supplies the only live mutation gateway, persisted commit epoch, maintenance admission, ordered projection barrier, and committed-publication fault semantics.
RFC 0022 makes dictionary additions roll back with a rejected or failed authoring transaction.
RFC 0013 should map stale, missing, unavailable, rejected, and operational failures consistently across frontends.
RFC 0016 should align workspace selection lifetimes with explicit authoring targets when both proposals are implemented.

## Goals

- Bind an edit to one runtime instance, one committed library revision, and one explicit stable target set.
- Reject an edit after any intervening effective library commit.
- Reject missing targets atomically instead of silently applying a subset.
- Keep storage authority and commit/publication in runtime while UIModel owns draft and stale-state policy.
- Return explicit applied, no-op, stale, missing, unavailable, rejected, and operational-failure outcomes.
- Make undo a new guarded authoring operation rather than unconditional replay.
- Ensure availability is published only after projections reflect the corresponding commit.
- Preserve the rule that metadata authoring changes the Aobus database, not source audio files.

## Non-goals

- Permit disjoint-field edits to commit concurrently against the same earlier snapshot.
- Write ID3, Vorbis Comment, MP4, or other tags back to source files.
- Persist editor drafts or undo history as library truth.
- Add collaborative or multi-writer conflict resolution.
- Make projection-local revision counters equal the library revision.
- Solve cross-process read-cache refresh.

## Proposed design

### Authoring availability

Runtime exposes one immutable availability value:

```text
Available { runtimeInstanceId, libraryRevision }
Maintenance { runtimeInstanceId, lastCommittedRevision, operationKind }
Faulted { runtimeInstanceId, lastCommittedRevision }
```

The persisted `libraryRevision` is the coarse authoring epoch.
Every effective coordinator-owned commit advances it; aborts, previews, rejected operations, and semantic no-ops do not.
No second epoch counter is maintained.

Scan apply, import, relink, and identity backfill are maintenance operations.
There is no field-disjoint identity-backfill exception.
Interactive authoring is unavailable for their complete lifetime and reopens only after their final committed change has passed the projection publication barrier.

### Runtime binding evidence

UIModel asks runtime to bind the explicit targets when an edit draft begins.
Runtime returns an opaque `BoundTrackTargets` value containing:

```text
runtime instance id
observed library revision
stable TrackIds
```

Construction is runtime-controlled.
The value is evidence, not permission: commit rechecks the current runtime instance, availability, revision, and every target while holding coordinator writer ownership.

Track ids are copied at bind time and never replaced with the currently focused selection at submit time.
An inline cell binds the row identity when editing starts; a detail editor or properties dialog binds its explicit selection when the draft opens.

### UIModel edit session

UIModel owns a platform-neutral edit session containing the runtime binding, validated draft, dirty state, stale state, and optional undo receipt.
It observes runtime availability but does not inspect storage or construct transactions.

Any availability event for another runtime instance, any later available revision, maintenance entry, or fault marks an open dirty session stale.
The frontend renders that state and may offer refresh or discard, but cannot override runtime validation.

Frontends bind widget/terminal events to the UIModel session.
They do not supply a commit callback that can bypass the session with raw ids.

### Atomic validation and commit

One metadata/tag submission performs:

1. reject a different runtime instance;
2. reject maintenance or faulted availability;
3. reject a binding revision different from the coordinator revision;
4. begin one library write transaction under coordinator ownership;
5. load every bound target and reject the complete command if any is missing;
6. validate and prepare every field, tag, and dictionary mutation;
7. return `NoOp` without commit when no target changes;
8. commit all changed targets and dictionary rows together;
9. publish one matching `LibraryChangeSet`; and
10. return applied evidence and a binding at the new committed revision.

The writer never silently skips a missing target for authoring.
Administrative best-effort behavior, if retained, uses a separately named offline command and is not callable from UIModel or a frontend.

### Typed outcomes

The runtime result separates semantic outcomes from operational errors:

```text
Applied { reply, libraryRevision, nextBinding }
NoOp
Stale
Missing { targetIds }
Unavailable
Rejected { validation error }
Failed { pre-commit operational error }
```

`Rejected` and `Failed` travel through the normal typed error channel.
The remaining cases are explicit authoring outcomes.

A failure after durable commit is not `Failed`.
RFC 0003 faults the runtime and reports that committed-publication condition through the runtime failure boundary.

### Guarded undo

An applied operation may return the values needed by UIModel to construct a reverse patch plus `nextBinding`, which is bound to the new committed revision.
Undo submits that reverse patch as a new guarded command.

Any intervening effective library commit makes the receipt stale, even when it changed an unrelated field.
This conservative rule prevents destructive replay without introducing per-field compare state.
Multi-target undo remains all-or-none.

### Projection ordering

Projection revisions remain projection-owned and are not embedded into the authoring binding.
The load-bearing contract is instead:

```text
commit revision R
  -> publish LibraryChangeSet R
  -> synchronously apply all authoring-relevant source/projection reducers
  -> publish Available(runtimeInstanceId, R)
```

An editor may refresh and bind revision `R` only after observing state derived from that same publication boundary.
A future asynchronous projection must participate in the RFC 0003 acknowledgement barrier.

### Ownership and source-file boundary

- Core library owns physical record, transaction, and dictionary integrity.
- Runtime owns binding construction, admission, target revalidation, commit, publication, and typed outcomes.
- UIModel owns edit-session, draft, stale, validation-presentation, and undo-history policy.
- GTK and TUI own widget/terminal lifetime and rendering only.

No command in this proposal writes through `media::file::File`.
Source-file authoring requires a separate transactional file-consistency proposal.

## Alternatives

### Per-field expected values

This permits disjoint concurrent edits but requires a potentially large per-target baseline, special mixed-value rules, and compare logic for every evolving field family.
The one-writer product model does not justify that complexity today.

### Disable only while scan writes a batch

Reopening authoring during scan preparation allows a plan or identity result to become stale between batches and creates user-visible admission flicker.
One maintenance interval is easier to reason about and express mechanically.

### Guard only on projection revision

Projection revisions have local owners and may change for selection or presentation reasons without a library commit.
They also cannot identify an old runtime after active-library replacement.

### Last writer wins

This silently loses edits and makes undo destructive.

### Frontend re-read immediately before submit

State can change again before commit, and every frontend would duplicate admission policy.
Validation belongs under the coordinator's transaction boundary.

## Compatibility and migration

The physical database layout does not change.
Runtime and UIModel APIs intentionally replace ids-plus-patch authoring calls; no compatibility overload remains available to interactive frontend code.

Existing editor drafts and undo values are ephemeral and can be discarded.
CLI mutation commands bind their selected ids immediately before submission or use an explicitly offline administrative composition.

## Validation

- Binding records one runtime instance, the current committed revision, and exact target order.
- A current binding commits and returns the next revision only after projection reducers have run.
- Any intervening effective metadata, tag, list, track, scan, import, relink, or identity commit yields `Stale` and changes nothing.
- A missing target yields `Missing` and no target or dictionary row changes.
- Multi-selection authoring is all-or-none.
- Semantic no-op does not commit, publish, or invalidate another open binding.
- Changing focused selection or recycling a GTK row cannot retarget an open editor.
- Active-library replacement rejects the old runtime binding before touching the replacement library.
- Maintenance and fault states reject authoring mechanically.
- Undo succeeds only with its returned next binding and becomes stale after any later commit.
- UIModel tests own draft/stale/undo policy; GTK tests cover only binding and lifecycle adaptation.
- Source audio fixture bytes remain unchanged after metadata and tag authoring.
- Concurrency, ThreadSanitizer, documentation, and full `./ao check` gates pass.

## Open questions

None.

## Promotion plan

- Update [library architecture](../architecture/library.md) and [presentation architecture](../architecture/presentation.md) with the runtime binding and UIModel edit-session boundaries.
- Update the [metadata-editing specification](../spec/presentation/metadata-editing.md) with availability, staleness, outcomes, atomicity, and undo behavior.
- Update GTK track-detail/dialog specifications with stale editor presentation and row-binding lifetime.
- Update library mutation and publication specifications with the projection barrier inherited from RFC 0003.
- Update CLI specification/reference if guarded authoring becomes part of its automation protocol.
