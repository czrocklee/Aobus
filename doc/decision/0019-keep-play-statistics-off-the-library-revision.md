---
id: decision.0019.keep-play-statistics-off-the-library-revision
status: accepted
---
# Decision 0019: keep play statistics off the library revision

## Context

The October 2026 review of library format version 10 found that Aobus records no play history.
There is no statistics store, the playback events carry no played event, and the AppKit media-remote adapter disables its rating command.
Deferring the feature loses only plays made before it ships, and losing that pre-release history is acceptable.
Under [closed admission](0008-close-library-admission-and-trust-live-storage.md), a later statistics store is a format version change, but adding an empty named database is cheap once a migration framework exists.
The store itself can therefore wait.

How plays interact with the library revision would be expensive to change after release, because consumers would come to rely on it.
Every library write that commits advances the revision, as the [library database reference](../reference/library/storage/database.md#metadata-records) defines it.
A Track [authoring binding](../system/presentation/metadata-editing.md#terminology) names one committed revision, and a later effective commit makes an open editor's submission `Stale`.
The live-runtime write lane admits [one interactive command and one background mutation](0015-sequence-live-runtime-library-writes.md) and answers further work with `Busy` or `ResourceBusy`.
A play recorded as an ordinary library write at every Track change would keep invalidating edits and occupying the lane while music plays.

## Decision

Recording a play, a skip, or a last-played time never advances the library revision and never occupies the live-runtime write lane.
When play statistics are added, they are stored apart from the revisioned records, written without a revision, and not published as a library mutation.

A rating is an explicit user edit rather than playback history, so this decision does not govern it.

## Alternatives considered

### Record plays as ordinary library writes

Rejected.
Whether a play rewrote the Track's record or wrote a separate row, each one would advance the revision.
Every Track change would then stale open editors and contend with the user's edits for the write lane.

### Batch plays into periodic revisioned writes

Rejected.
Fewer revisions do not help an editor, whose submission would still turn `Stale` at a moment unrelated to anything the user did.

## Consequences

- A statistics store needs its own durability and notification rules; a consumer that shows play counts cannot rely on revision publication to refresh.
- The store waits for the migration framework, because it adds a named database to an exactly admitted catalog.
- Plays made before the feature ships are not recorded.
