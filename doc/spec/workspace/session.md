---
id: workspace.session
type: spec
status: current
domain: workspace
summary: Defines workspace snapshot capture, best-effort checkpointing, candidate restoration, fallback, and initial-history behavior.
---
# Workspace session

## Scope

This specification defines current save and restore behavior for the runtime workspace session.
It owns snapshot capture from open views, use of the `workspace` configuration group, candidate-view preparation, restore commit, active-view fallback, custom-preset restoration, and initial navigation history.

It does not define exact serialized fields, generic `ConfigStore` behavior, managed file paths, playback-session restoration, GTK window preferences, or presentation semantics.
Those facts belong to the linked reference, persistence, playback, and presentation owners.

## Code boundary

Workspace session behavior belongs to the **application runtime** layer in the [system architecture](../../architecture/system-overview.md), under the [workspace architecture](../../architecture/workspace.md).
The public owner is `WorkspaceService::saveSession` and `restoreSession` in `app/include/ao/rt/WorkspaceService.h`; the candidate model is `WorkspaceSessionState`; the implementation is `app/runtime/WorkspaceService.cpp`.

The runtime owns session meaning and validation while `ConfigStore` supplies grouped-file mechanics under the [persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md).
Frontends inject and schedule the store but do not decode workspace fields.

## Terminology

- **Live workspace** is the current open/focused view aggregate plus custom presentation presets.
- **Session snapshot** is one `WorkspaceSessionState` captured from the live workspace.
- **Candidate view** is a view created during restore before the restored aggregate is installed.
- **Active-list hint** is the persisted `ListId` used to choose a focused restored view without persisting `ViewId`.
- **Checkpoint** is the current one-shot group-save attempt at an explicit frontend lifecycle point.

## Invariants

- Workspace session state is associated with one selected library and is not global application preference state.
- A session never persists `ViewId`, source leases, projection rows, selection, navigation history, scroll position, widget state, or playback state.
- Every persisted open view is reconstructed against the active runtime's library and source cache.
- Restore does not install candidate view ids into the workspace until every candidate view has been created successfully.
- If candidate creation fails, all candidates created by that restore attempt are destroyed before the error returns.
- A missing `workspace` group is a successful empty restore.
- The active-list hint is advisory; restore falls back to the first restored view when no candidate matches it.
- Successful restore commits the focused restored view as one initial navigation point and does not restore an old back/forward stack.
- Workspace custom presentation presets are captured and restored with the workspace session, independently from GTK per-list presentation preferences.
- Playback-session state remains a separate payload and semantic owner even when it shares a physical `ConfigStore` with workspace state.

## State model

The live workspace contains ordered open `ViewId` values, one active `ViewId`, a workspace revision, navigation history, and custom presets.

The persisted candidate contains ordered semantic `TrackListViewConfig` values, one active-list hint, and custom presets.
Each view config carries its base list, filter, legacy group/sort fields, and optional exact presentation.
The [workspace session state reference](../../reference/workspace/session-state.md) owns that exact shape and current unversioned encoding.

## Commands and transitions

### Capture and save

`saveSession(store)` snapshots open views in workspace order.
For each live view it records the base list, filter expression, group and sort state, and exact presentation.
When the view is active, its base `ListId` becomes the active-list hint.
The snapshot also copies all custom presets.

The current implementation calls the result-bearing `ConfigStore::save("workspace", snapshot)` operation synchronously.
A load, encode, emission, or replacement failure leaves the prior document unchanged; recoverable failure is logged and the void command returns normally.
There is no dirty revision, retry schedule, or durable acknowledgement for workspace state.

### Load and prepare

`restoreSession(store)` default-constructs a candidate and performs ordinary group decoding.

- A missing group returns success without creating a view or changing workspace state.
- A file, parse, node-shape, or aggregate-decode failure returns the store's recoverable error.
- Successful decode attempts to create every listed view as attached through `ViewService`.
- A view creation failure destroys all views created earlier in the same attempt and returns that failure.

Candidate creation may allocate runtime `ViewId` values that are later destroyed on failure.
Those ids are not exposed through the workspace aggregate before commit.

### Commit and focus

After every candidate view exists, restore adds them to the workspace in serialized order and replaces the custom-preset collection with the restored values.

Focus resolution scans restored views for the active-list hint.
A matching unfiltered view is a candidate, but a later matching filtered view wins immediately; this lets a filtered view for the active list take precedence over an unfiltered view.
If no match exists and at least one view was restored, the first open view becomes active.
An empty candidate leaves the workspace without an active view.

Restore then commits the active semantic view as the initial navigation point.
This initial point is deduplicated normally and has no previous entry.

### Frontend lifecycle integration

GTK restores workspace state after reconstructing library-backed pages and before restoring playback state.
It applies GTK per-list presentation preferences after workspace restore.
If no view is open, GTK navigates to All Tracks with the resolved default presentation and immediately requests a workspace checkpoint.

GTK requests workspace save together with its global window/session, presentation, and playback checkpoints during explicit save, hide, ordinary shutdown, and active-library preparation.
Exact active-library transitions belong to the [GTK active-library lifecycle specification](../linux-gtk/active-library-lifecycle.md).

TUI currently injects a workspace `ConfigStore` into `AppRuntime` but does not call workspace restore or save around its event loop.
Its `LibraryController` constructs an initial All Tracks workspace view instead.
[RFC 0018](../../rfc/0018-interactive-session-lifecycle.md) proposes one startup, checkpoint, and shutdown state machine for both interactive frontends.

## Failure and cancellation

Restore returns `Result<>` and preserves the pre-commit workspace aggregate when store decoding or candidate view creation fails.
Candidate cleanup releases their projections and source leases through `ViewService::destroyView`.

Ordinary decoding is permissive: missing aggregate fields retain defaults and invalid elements inside decoded vectors may be skipped by the shared codec.
The exact consequences belong to the [workspace session state reference](../../reference/workspace/session-state.md) and [grouped configuration store specification](../persistence/config-store.md).

Save is currently best effort.
The workspace wrapper does not return the store failure, so a failed checkpoint cannot block a library switch or shutdown transition.
The grouped store nevertheless keeps the previous live document and backing bytes unchanged when its candidate save fails.
[RFC 0015](../../rfc/0015-fail-closed-config-store.md) records why a larger transaction, recovery, and commit-receipt system was rejected after that narrower store boundary was implemented.

Save and restore are synchronous and expose no cancellation point.
They run under the runtime/frontend serialized application-control boundary.

## Persistence and versioning

The session uses the literal `workspace` group in the `ConfigStore` owned by `AppRuntime`.
GTK places that store at its per-library workspace location; TUI uses its selected configuration path.
Exact paths belong to the [managed file locations reference](../../reference/persistence/location.md).

The current payload has no schema version, migration, aliases, or stable field-name layer separate from reflected C++ member names.
Enum values use current numeric encodings.
Compatibility details and the exact field inventory belong to the [workspace session state reference](../../reference/workspace/session-state.md); proposed versioning is tracked by [RFC 0010](../../rfc/0010-versioned-presentation-state.md).
[RFC 0017](../../rfc/0017-versioned-workspace-session.md) proposes the workspace root envelope, library binding, exact active-view reference, limits, and transactional restore that complement RFC 0010's nested presentation codec.

## Frontend observations

Workspace restore publishes ordinary focus and custom-preset observations during commit.
It does not publish a distinct session-restored event or progress state.

A successful empty restore is distinguished from a restored non-empty workspace only by the resulting workspace snapshot.
GTK uses that snapshot to decide whether to create the default All Tracks view.

Workspace save publishes no success or failure event.
Current recoverable save failure is diagnostic logging only.

## Implementation map

- [`WorkspaceService`](../../../app/include/ao/rt/WorkspaceService.h) exposes the session commands.
- [`WorkspaceSessionState`](../../../app/include/ao/rt/WorkspaceSessionState.h) defines the candidate aggregate.
- [`WorkspaceService.cpp`](../../../app/runtime/WorkspaceService.cpp) captures, prepares, commits, falls back, and seeds history.
- [`ViewService`](../../../app/include/ao/rt/ViewService.h) creates and destroys candidate views.
- [`ConfigStore`](../../../app/include/ao/rt/ConfigStore.h) provides candidate group decoding and one-shot atomic group save.
- [`MainWindowCoordinator.cpp`](../../../app/linux-gtk/app/MainWindowCoordinator.cpp) owns current GTK restore/default/checkpoint sequencing.
- [`app/tui/App.cpp`](../../../app/tui/App.cpp) owns the current TUI store injection without session lifecycle calls.

## Test map

- [`WorkspaceSessionTest.cpp`](../../../test/unit/runtime/WorkspaceSessionTest.cpp) proves missing-group success, restored initial history, fallback, malformed rejection, candidate cleanup, and current save-failure tolerance.
- [`HeadlessShellTest.cpp`](../../../test/unit/runtime/HeadlessShellTest.cpp) proves cross-runtime open-view, active-view, grouping, sorting, and presentation reconstruction.
- [`WorkspaceHistoryTest.cpp`](../../../test/unit/runtime/WorkspaceHistoryTest.cpp) protects the history behavior seeded by restore.
- [`MainWindowCoordinatorTest.cpp`](../../../test/unit/linux-gtk/app/MainWindowCoordinatorTest.cpp) protects GTK workspace/playback restore composition and checkpoint interactions.
- [`MainWindowTest.cpp`](../../../test/unit/linux-gtk/app/MainWindowTest.cpp) protects lifecycle save triggers and switch preparation.

## Related documents

- [Workspace architecture](../../architecture/workspace.md)
- [Workspace navigation](navigation.md)
- [Workspace session state](../../reference/workspace/session-state.md)
- [Persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md)
- [Grouped configuration store](../persistence/config-store.md)
- [Application managed-state surface](../../reference/persistence/application-config.md)
- [List presentation preference](../presentation/list-preference.md)
- [Playback architecture](../../architecture/playback.md)
