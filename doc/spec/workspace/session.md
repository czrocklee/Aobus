---
id: workspace.session
type: spec
status: current
domain: workspace
summary: Defines workspace snapshot capture, candidate restoration, one-commit installation, fallback, and initial history.
---
# Workspace session

## Scope

This specification defines current save and restore behavior for the runtime workspace session.
It owns capture from live views, use of the `workspace` configuration group, candidate-view preparation, atomic aggregate installation, active-view fallback, custom-preset restoration, and initial history.

It does not enumerate serialized fields, generic `ConfigStore` mechanics, managed paths, playback-session restoration, GTK preferences, or presentation semantics.

## Code boundary

Workspace session behavior belongs to the **application runtime** layer from the [system architecture](../../architecture/system-overview.md), as refined by the [workspace architecture](../../architecture/workspace.md) and [persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md).
`WorkspaceService::saveSession()` and `restoreSession()` are the public owner, `WorkspaceSessionState` is the decoded semantic candidate, and `WorkspaceSnapshot` is the live committed aggregate.
The private `WorkspaceSessionDocument` codec owns strict persisted structure and stable presentation conversion.

`ConfigStore` supplies candidate group decoding and atomic whole-document saves.
Frontends choose lifecycle points and inject the store but do not decode workspace fields.

## Terminology

- **Live workspace** is the current revisioned workspace snapshot plus the view-local state identified by its open ids.
- **Session snapshot** is one `WorkspaceSessionState` captured from that live state.
- **Candidate view** is a view created during restore before its id is installed in the workspace snapshot.
- **Active-list hint** is the persisted `ListId` used to choose a focused candidate without persisting `ViewId`.
- **Checkpoint** is one synchronous save attempt at a frontend lifecycle point.

## Invariants

- Workspace session state belongs to one selected library and is not global preference state.
- A session never persists `ViewId`, source leases, projection rows, selection, history, scroll, widget, or playback state.
- Every persisted view is reconstructed against the active runtime's library and source cache.
- Restore publishes no workspace aggregate until every candidate view has been created.
- A failed candidate creation destroys every view created by that restore and preserves snapshot, revision, focus, presets, and history.
- A missing `workspace` group is a successful `NoChange` restore.
- One successful nonempty restore advances one workspace revision and publishes one complete `Restore` observation.
- The active-list hint is advisory; unmatched state falls back to the first open view.
- The restored active view becomes one initial navigation point; old history is never persisted.
- Custom presentation presets are captured and installed in the same workspace commit as restored views and focus.
- Playback-session state remains a separate semantic owner even when it shares a physical store.

## State model

The live `WorkspaceSnapshot` contains ordered open ids, active id, custom presets, navigation availability, and workspace revision.
Each open id identifies `TrackListViewState` owned by `ViewService`.

The persistence candidate contains ordered semantic `TrackListViewConfig` values, one active-list hint, and custom presets.
Each decoded config carries base list, filter, and a required exact presentation; the derived group and sort fields mirror that presentation for view creation.
The [workspace session state reference](../../reference/workspace/session-state.md) owns the exact current encoding.

## Commands and transitions

### Capture and save

`saveSession(store)` copies the current workspace snapshot and walks its open views in order.
For each live view it records list, filter, group, sort, and exact presentation.
The active view contributes its base list as the active-list hint.
The session also copies the snapshot's complete custom-preset collection.

The command first converts the semantic state to one canonical `WorkspaceSessionDocument`, then calls `ConfigStore::save("workspace", document)` once.
A load, encode, emission, or atomic-replacement failure leaves the previous document unchanged; the workspace wrapper logs that recoverable failure and returns normally.
There is no workspace dirty revision, retry scheduler, or durable acknowledgement.

### Load and prepare

`restoreSession(store)` default-constructs a persistence document, asks the store for strict recursive decoding of the `workspace` group, and converts the complete document to one semantic candidate.

- A missing group returns a `NoChange` receipt without creating a view or publishing an event.
- File, parse, node-shape, missing/extra field, aggregate/vector-decode, version, or stable-vocabulary failure returns before view creation.
- Successful decode attempts to create every configured view through `ViewService`.
- If a later creation fails, all earlier candidate ids are destroyed and the error returns.

View creation is synchronous and emits no creation observation.
Candidate ids may exist briefly inside `ViewService`, but they do not enter `WorkspaceSnapshot` or any workspace event before commit.

### Prepare focus and history

After all candidate views exist, restore prepares one copy of the current workspace snapshot and history.
Candidate ids are appended in serialized order and the custom-preset collection is replaced by the restored collection.

Focus resolution scans the restored candidates for the active-list hint.
A matching unfiltered view remains a fallback while a later matching filtered view wins immediately.
If no restored candidate matches and any view is open, the first open view is focused.
This preserves the current append behavior when restore is called on a nonempty workspace; [RFC 0017](../../rfc/0017-versioned-workspace-session.md) proposes explicit replacement and exact active-view identity.

The final active semantic view is committed into the history candidate.
That initial point is deduplicated normally and has no previous entry in the ordinary empty-runtime restore path.

### Commit

Restore installs open ids, focus, complete custom presets, navigation availability, history cursor, and revision through one workspace commit.
The returned `WorkspaceCommitReceipt` identifies the before and after revisions and resulting active view.
An effectively identical decoded candidate returns `NoChange`.

One `WorkspaceChanged` event with cause `Restore` is queued after acceptance.
Consumers never receive a sequence of partially added views or a separate preset/focus event.

## Frontend lifecycle integration

GTK restores workspace state after constructing the library-bound runtime and before restoring playback state.
It applies GTK per-list presentation preferences after workspace restore.
If the restored snapshot has no open view, GTK navigates to All Tracks with the resolved default presentation and immediately requests a workspace checkpoint.
That automatic first-view checkpoint occurs only after a successful empty restore; a rejected workspace document may produce an in-memory default view but is not overwritten during initialization.

GTK saves workspace state with its other explicit save, hide, shutdown, and active-library preparation checkpoints.
The [GTK active-library lifecycle specification](../linux-gtk/active-library-lifecycle.md) owns those transitions.

TUI currently injects a workspace store but does not restore or save it around the event loop.
Its `LibraryController` constructs an initial All Tracks view instead.
[RFC 0018](../../rfc/0018-interactive-session-lifecycle.md) proposes a shared interactive lifecycle owner.

## Failure, execution, and lifetime

Save and restore are synchronous callback-executor commands with no cancellation point.
Restore errors before commit preserve the live workspace aggregate and history.
Candidate cleanup releases projections and source leases through `ViewService::destroyView()`.

Workspace persistence uses exact aggregate/vector membership plus semantic presentation validation.
Missing or extra fields and malformed vector elements reject the complete document instead of retaining defaults or being skipped.
Unsupported `presentationVersion` values and unknown closed presentation tokens are `FormatRejected`.

Resource limits, full root versioning, library binding, exact active-view identity, and recovery remain [RFC 0017](../../rfc/0017-versioned-workspace-session.md) concerns.

Save remains best effort at the workspace wrapper.
Its lower `ConfigStore` operation is fail closed, but a failed checkpoint does not currently block shutdown or library replacement.

## Persistence and versioning

The session uses the literal `workspace` group in the store owned by `AppRuntime`.
GTK uses a per-library workspace location; TUI uses its selected configuration path.
Exact locations belong to the [managed file locations reference](../../reference/persistence/location.md).

The payload carries required `presentationVersion: 1` and stable textual field, sort, group, and direction values.
That marker covers nested presentation vocabulary only.
The payload still has no complete root schema version, library identity, migration aliases, resource budgets, or exact persisted active-view identity.
The [workspace session state reference](../../reference/workspace/session-state.md) owns the exact inventory and compatibility boundary.

## Implementation map

- [`WorkspaceService`](../../../app/include/ao/rt/WorkspaceService.h) captures, prepares, commits, and reports session operations.
- [`WorkspaceSnapshot`](../../../app/include/ao/rt/WorkspaceSnapshot.h) is the one live aggregate installed by restore.
- [`WorkspaceSessionState`](../../../app/include/ao/rt/WorkspaceSessionState.h) is the persistence candidate.
- [`WorkspaceSessionCodec`](../../../app/runtime/WorkspaceSessionCodec.h) owns the private strict document and stable presentation conversion.
- [`ViewService`](../../../app/include/ao/rt/ViewService.h) creates and destroys candidate views.
- [`ConfigStore`](../../../app/include/ao/rt/ConfigStore.h) supplies candidate decoding and one-shot save.
- [`MainWindowCoordinator.cpp`](../../../app/linux-gtk/app/MainWindowCoordinator.cpp) owns current GTK restore/default/checkpoint sequencing.

## Test map

- [`WorkspaceSessionTest.cpp`](../../../test/unit/runtime/WorkspaceSessionTest.cpp) proves missing-group `NoChange`, one-event multi-view restore, initial history, fallback, malformed rejection, and candidate cleanup.
- [`WorkspaceSessionCodecTest.cpp`](../../../test/unit/runtime/WorkspaceSessionCodecTest.cpp) proves canonical stable vocabulary, semantic round trip, and invalid-document rejection.
- [`HeadlessShellTest.cpp`](../../../test/unit/runtime/HeadlessShellTest.cpp) proves cross-runtime view and presentation reconstruction.
- [`WorkspaceHistoryTest.cpp`](../../../test/unit/runtime/WorkspaceHistoryTest.cpp) protects the history seeded by restore.
- [`MainWindowCoordinatorTest.cpp`](../../../test/unit/linux-gtk/app/MainWindowCoordinatorTest.cpp) protects GTK workspace/playback restore composition.

## Related documents

- [Workspace architecture](../../architecture/workspace.md)
- [Workspace navigation](navigation.md)
- [Workspace session state](../../reference/workspace/session-state.md)
- [Persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md)
- [Grouped configuration store](../persistence/config-store.md)
- [GTK active-library lifecycle](../linux-gtk/active-library-lifecycle.md)
