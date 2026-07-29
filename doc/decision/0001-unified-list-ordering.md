---
id: decision.0001.unified-list-ordering
type: decision
status: accepted
domain: library
summary: Unifies saved Lists around predicate membership plus optional rank, preserves hidden rank, and uses committed revision as order-write authority.
---
# Decision 0001: unify saved Lists with an independent order overlay

## Context

Aobus is centered on tag- and expression-based collection.
The previous persisted Manual, Smart, and Folder distinctions made that model compete with a second copied-membership model, while presentation sorting already supplied most nonmanual order semantics.
Users still need an authored sequence for Playlist-like listening, and that sequence must survive switching to album, artist, year, or custom presentations.

The July 2026 unified List ordering design and its reviewer exchange accepted this decision before the version-5 implementation was activated.
That review specifically challenged hidden-rank retention and global revision binding.
The final verdict withdrew both objections after comparing their observable alternatives, while requiring explicit Remove wording, shared preference cleanup, maintenance feedback, compiled-and-tested intermediate changes, and documented keyboard repeat behavior.

## Decision

Every persisted saved List has one shape:

- `parentId` selects its upstream source;
- `filter` is a local predicate, with empty text meaning identity;
- `orderTrackIds` is an optional rank overlay and never membership;
- name and description remain ordinary metadata.

There is no persisted Manual, Smart, Folder, or Playlist kind.
Nesting always means source derivation.
**New Playlist...** is a tag-backed authoring template that creates an ordinary List and chooses the `list-order` presentation preference.

Effective source order is:

1. current members found in `orderTrackIds`, in raw rank order;
2. current unranked members, in filtered parent order.

Raw ranked IDs outside current membership are retained as hidden ranks.
An ordinary predicate, parent, or tag edit does not prune them.
Explicit **Remove from Playlist** removes the membership tag and forgets the selected List positions atomically; **Forget Hidden Positions** is the separate bulk-prune command.
The user-visible Remove result must say that positions were forgotten.

The first effective move lazily materializes all current members into rank and rewrites the complete List value.
No-op moves do not materialize or commit.
Presentation `sortBy` remains the final visible and playback ordering authority: an empty sort exposes source order, while a nonempty sort leaves saved rank intact but currently hidden.
Grouping and relative moves through a transient quick filter are not writable order surfaces.
Absolute **Move to Top** and **Move to Bottom** remain available through a transient quick filter because they are defined against the complete effective sequence, not the filtered projection.

The virtual All Tracks source owns no rank record.
A user who wants a whole-library authored order creates a root saved List with an identity filter.

An order gesture binds runtime instance, committed library revision, List id, and complete effective TrackId sequence.
The writer rechecks runtime identity, availability, committed revision, and List existence under its own mutation authority.
Frontend view/source generation cancels a gesture early but is not transaction proof.
Maintenance is a discrete unavailable state and must produce visible “library busy” feedback.

List storage uses the version-5 canonical 20-byte header.
Count and text lengths are 32-bit, and data offsets are derived rather than persisted redundantly.
The exact byte diagram remains owned by the [database reference](../reference/library/storage/database.md).

## Alternatives considered

### Retain Manual, Smart, Folder, and Playlist kinds

Rejected because expression membership and authored order are orthogonal.
Kinds create branching source, editor, navigation, transfer, and lifecycle behavior without adding user capability.
A Folder would also give tree indentation two meanings: sometimes derivation, sometimes visual organization.
If large List collections become hard to navigate, search, pinning, recency, or UI-only collections can address that independently.

### Store order in the presentation

Rejected because a presentation describes reusable shape: grouping, sorting, fields, and suppression.
Embedding TrackIds there would couple a reusable view spec to one List and make switching presentations lose or duplicate authored sequence.

### Give All Tracks a writable order

Rejected because All Tracks is a virtual system source rather than a saved List record.
Persisting an unbounded global rank adds large automatic state and confuses library identity with user curation.
An identity-filter root List provides the same user outcome explicitly.

### Prune hidden ranks on every reorder

Rejected after review.
It makes an unrelated move of one visible track silently and permanently forget every currently hidden position.
Whether a returning track recovers its place would then depend on whether an intervening reorder happened, which is less explainable than explicit hidden rank.
It also does not remove the raw/effective distinction caused by lazy materialization, and hidden IDs would still exist between reorders.

### Prune rank immediately when membership changes

Rejected because ordinary metadata edits can temporarily remove a track.
Immediate pruning would break the existing useful behavior that restoring membership restores position.
Explicit Playlist removal remains available when departure should also forget rank.

### Use source generation as writer authority

Rejected because source and view replicas are downstream of committed storage and cannot prove an LMDB transaction current.
A per-List generation would also need a new persisted or coordinator-owned consistency authority.
Committed revision already serializes semantic mutations and protects the full effective-sequence evidence.
Generation remains valuable as an earlier UI cancellation signal.

### Eager materialization or sparse anchors

Rejected because eager materialization writes large values for Lists that may never be manually arranged.
A sparse TrackId vector cannot independently represent every total order and becomes coupled to a changing implicit parent tail.
Lazy full materialization pays the complete rewrite only on an effective authored move and keeps semantics deterministic.

### Persist explicit offsets or a 32-byte header

Rejected because canonical packing derives every offset from one count and three lengths.
Redundant offsets add validation and corruption states without adding capability.

## Consequences

- Membership has one authority: parent composition plus predicate evaluation.
- Any saved List can retain manual rank and later expose it through any flat unsorted presentation.
- Switching presentations never destroys rank, and playback reconstructs the same order as the view.
- Hidden ranks and unranked tails are intentional concepts that transfer, mutation, source, UI, and tests must distinguish.
- Each effective move is an O(List record size) rewrite; OS auto-repeat is suppressed so one held key does not issue repeated full rewrites.
- A global unrelated commit can stale a gesture, but maintenance is explicitly unavailable and frontend generation usually cancels changed views earlier.
- Renaming a Playlist does not rename its ordinary membership tag.
- Ordinary deletion rejects dependents; explicit cascade previews and atomically deletes the subtree.
- Every deleted List id must clear its presentation preference through the shared UIModel lifecycle.
- Database version 5 and YAML version 3 intentionally reject their predecessors; there is one activation boundary rather than repeated user-visible migrations.
- Intermediate implementation changes must compile and receive unit coverage while remaining unreachable from shipping frontend composition until the single activation point; they are not excluded from compilation with long-lived conditional code.

## Current authorities

- [Library architecture](../architecture/library.md)
- [Track expression architecture](../architecture/track-expression.md)
- [Presentation architecture](../architecture/presentation.md)
- [List model](../reference/library/model/list.md)
- [Library database](../reference/library/storage/database.md)
- [Library YAML format](../reference/library/format/yaml.md)
- [Track sources](../spec/library/source/track-source.md)
- [Library mutations](../spec/library/runtime/mutation.md)
- [Track-list presentation](../spec/presentation/track-presentation.md)
- [List presentation preference](../spec/presentation/list-preference.md)
- [Organize music with Lists and Playlists](../user/organize-with-lists.md)

## Supersession

Not superseded.
