---
id: workspace.session
---

# Workspace session

## Scope

This specification defines current save and restore behavior for the runtime workspace session.
It owns capture from live views, use of the `workspace` configuration group, candidate-view preparation, exact active-view selection, atomic aggregate installation, custom-preset restoration, and initial history.

It does not enumerate serialized fields, generic `ConfigStore` mechanics, managed paths, playback-session restoration, GTK preferences, or presentation semantics.

## Code boundary

Workspace session behavior belongs to the **application runtime** layer from the [system architecture](../overview.md), as refined by the [workspace architecture](README.md) and [persistence and managed-state architecture](../persistence/README.md).
`WorkspaceService::saveSession()` and `restoreSession()` are the public owner, `WorkspaceSessionState` is the deserialized semantic candidate, and `WorkspaceSnapshot` is the live committed aggregate.
The owner-local `WorkspaceSessionYamlSchema` owns explicit YAML structure, a private persistence document, stable presentation conversion, and pre-install semantic validation.

`ConfigStore` invokes that schema explicitly, reports group presence, and supplies atomic whole-document saves.
Frontends choose lifecycle points and inject the store but do not deserialize workspace fields.

## Terminology

- **Live workspace** is the current revisioned workspace snapshot plus the view-local state identified by its open ids.
- **Session snapshot** is one `WorkspaceSessionState` captured from that live state.
- **Candidate view** is a view created during restore before its id is installed in the workspace snapshot.
- **Active-view index** is the zero-based position of the focused semantic view in ordered `openViews`.
- **Checkpoint** is one synchronous save attempt at a frontend lifecycle point.

## Invariants

- Workspace session state belongs to one selected library and is not global preference state.
- A session saved for another library opens none of its views; only its custom presets, which name no library object, are restored.
- A session never persists `ViewId`, source leases, projection rows, selection, history, scroll, widget, or playback state.
- Every persisted view is reconstructed against the active runtime's library and source cache.
- Restore publishes no workspace aggregate until every candidate view has been created.
- A failed candidate creation destroys every view created by that restore and preserves snapshot, revision, focus, presets, and history.
- A missing `workspace` group is a successful no-op restore.
- One successful nonempty restore advances one workspace revision and publishes one complete `Restore` observation.
- A nonempty session identifies exactly one active entry by an in-bounds active-view index.
- An empty session uses active-view index `0` and never dereferences it as an entry.
- The restored active view becomes one initial navigation point; old history is never persisted.
- Custom presentation presets are captured and installed in the same workspace commit as restored views and focus.
- Playback-session state remains a separate semantic owner even when it shares a physical store.

## State model

The live `WorkspaceSnapshot` contains ordered open ids, active id, custom presets, and workspace revision.
Each open id identifies `TrackListViewState` owned by `ViewService`.

The persistence candidate contains ordered semantic `TrackListViewConfig` values, one active-view index, and custom presets.
Each deserialized config carries a base list, filter, and exact presentation. The presentation records the built-in or custom preset ID, the grouping and base sort, and any temporary header sort.
The [workspace session state reference](../../reference/workspace/session-state.md) owns the exact current serialization.

## Commands and transitions

### Capture and save

`saveSession(store)` copies the current workspace snapshot and walks its open views in order.
For each live view it records the list, filter, and exact presentation.
For a nonempty workspace, the active view contributes its position in that ordered sequence.
An empty workspace contributes the canonical index `0`.
A nonempty live snapshot must contain its active id. `saveSession()` deliberately seeds an out-of-range index until it encounters that id, so a broken live aggregate violates the serialization contract instead of being written as an ambiguous session.
The session also copies the snapshot's complete custom-preset collection and records the open library's id.

The command passes the captured semantic state and `WorkspaceSessionYamlSchema` to one `ConfigStore::save("workspace", state, schema)` call.
The schema converts through its private document and emits the canonical YAML subtree.
Returned conversion, emission, or atomic-replacement failures leave the previous document unchanged; the workspace wrapper logs that recoverable failure and returns normally. Violating the live-state serialization preconditions is a programming error, not a recoverable `InvalidState` save result.
There is no workspace dirty revision, retry scheduler, or durable acknowledgement.

### Load and prepare

`restoreSession(store)` seeds a `WorkspaceSessionState` and asks the store to load the `workspace` group through `WorkspaceSessionYamlSchema`.
The schema strictly deserializes its private document and returns one complete semantic candidate.

- A missing group returns success without creating a view or publishing an event.
- A load error other than `NotFound` returns before view creation. The store preserves the `workspace` group: a fallback view cannot overwrite the rejected session on shutdown. A successful explicit removal releases that protection; other groups in a readable document remain independently writable.
- An empty candidate requires active-view index `0`; a nonempty candidate requires an index smaller than `openViews.size()`.
- Index validation completes before the first candidate view is created.
- A candidate whose `libraryId` differs from the open library's id loses its views and active index before view creation, because its list ids were issued by another library; restore then continues as an empty candidate carrying the saved custom presets.
- Successful deserialize attempts to create every configured view through `ViewService`.
- If a later creation fails, all earlier candidate ids are destroyed and the error returns.

View creation is synchronous and emits no creation observation.
Candidate ids may exist briefly inside `ViewService`, but they do not enter `WorkspaceSnapshot` or any workspace event before commit.

### Prepare focus and history

After all candidate views exist, restore prepares one copy of the current workspace snapshot and history.
Candidate ids are appended in serialized order and the custom-preset collection is replaced by the restored collection.

For a nonempty candidate, focus is exactly `createdViewIds[activeViewIndex]`.
No list identity, filter shape, or presentation property participates in focus selection.
For an empty candidate, restore does not dereference the index: an empty aggregate remains unfocused, while restore onto an existing workspace selects its first existing open view.

The final active semantic view is committed into the history candidate.
That exact view, including filter and presentation, is deduplicated normally and has no previous entry in the ordinary empty-runtime restore path.

### Commit

Restore installs open ids, focus, complete custom presets, history cursor, and revision through one workspace commit.
The command returns empty success; callers observe restored state through `snapshot()`.
An effectively identical deserialized candidate succeeds without publication.

One `WorkspaceChanged` event with cause `Restore` is queued after acceptance.
Consumers never receive a sequence of partially added views or a separate preset/focus event.

## Frontend lifecycle integration

GTK restores workspace state after constructing the library-bound runtime and before restoring playback state.
Every restored view retains the exact presentation carried by the workspace candidate, and GTK does not reinterpret that state through its per-list default preference.
The restored active view therefore seeds history with the same presentation that remains visible after initialization; later history replay restores that exact point.
If the restored snapshot has no open view, GTK navigates to All Tracks with the resolved default presentation and immediately requests a workspace checkpoint.
That automatic first-view checkpoint occurs only after a successful empty restore. A rejected load leaves the stored group unchanged, including on a later checkpoint, as [failure](#failure-execution-and-lifetime) specifies. The session may still open an in-memory fallback view.

For ordinary list navigation and playback restoration, GTK always resolves the per-list preference or recommendation and submits it as `NewViewDefault` presentation intent.
`WorkspaceService` decides whether to reuse an existing unfiltered view or create one: reuse preserves the exact active presentation, while creation applies the supplied default.
A filtered view over the same list is not reusable for this purpose, retains its own exact presentation, and does not prevent creation of a plain view with the supplied default.

GTK saves workspace state at window hide, window destruction, application shutdown, and active-library preparation checkpoints.
The [GTK active-library lifecycle specification](../frontend/gtk/active-library-lifecycle.md) owns those transitions.

TUI restores the selected workspace store before constructing `LibraryController`.
Restore allocates process-local view ids, and the controller attaches the exact `WorkspaceSnapshot::activeViewId` from that runtime rather than navigating by a persisted id or by `ListId`.
This preserves the active member when several filtered views share one list, its exact presentation and grouping/sort state, restored custom presets, and the initial history point.
A successfully materialized empty projection is a usable active view, not a fallback condition.
Only an absent, closed, unprojectable, or otherwise unusable active view causes explicit navigation to All Tracks.

TUI reload materializes the existing active view id without navigation, preserving canonical filter and presentation state, any in-progress terminal filter draft, and list/presentation chooser highlights that may intentionally differ from the active view.
If that id disappeared, reload first follows a different active workspace id and otherwise navigates to All Tracks; list-deletion recovery therefore does not depend on library-observer registration order.
On normal exit, TUI cancels transient interactions before calling the same best-effort `saveSession()` command, then checkpoints playback and stops it.
A workspace save failure remains log-only and does not block teardown.
No shared frontend lifecycle owner is introduced; GTK and TUI keep explicit composition-root sequences.

WinUI checkpoints workspace state through its desktop-settings composite: the best-effort workspace checkpoint runs first, and the checked desktop-settings save that follows is the composite's only result and alone gates durable library-root admission (see the [Windows frontend](../frontend/windows.md)).
A failed root commit keeps the earlier workspace checkpoint written, reports the failure through the session's notification and status surfaces, and does not block the successor session.
A successful WinUI header sort also requests a separate coalesced workspace save. Repeated requests before the deferred callback runs become one save of the live workspace, and that save does not write desktop settings. Session shutdown disarms the callback before the final workspace checkpoint. A refused sort does not request the save.
Session teardown additionally checkpoints the workspace best effort before the runtime graph shuts down.

## Failure, execution, and lifetime

Save and restore are synchronous callback-executor commands with no cancellation point.
Restore errors before commit preserve the live workspace aggregate and history.
Candidate cleanup releases projections and source leases through `ViewService::destroyView()`.

Workspace persistence uses an explicit recursively strict schema plus semantic presentation validation.
Missing or extra fields and malformed vector elements reject the complete document instead of retaining defaults or being skipped.
Unsupported `presentationVersion` values return `NotSupported` before version-specific siblings are interpreted; unknown closed presentation tokens are `FormatRejected`.

A load failure other than a missing group calls `ConfigStore::preserveGroup("workspace")` on that store. Later `save` and `saveTogether` that include the group return `InvalidState` and write nothing until a successful `removeGroup("workspace")`. Sibling groups on the same store remain writable. The mark lives on that store instance; another store opened on the same file does not inherit it.

This retains a failed decode, including an unsupported presentation version and a malformed document. It does not retain a successful decode whose list ids later fail view creation. It is not protected configuration: the group is not reported by `hasNewerGroup`, removal is allowed, and the refusal is not `NotSupported`. Workspace remains losable state. There is no migration and no read compatibility with an older vocabulary. A refused checkpoint is logged and does not block shutdown.

The current schema has no resource limits or full root version.

Save remains best effort at the workspace wrapper.
Its lower `ConfigStore` operation is fail closed, but a failed checkpoint does not currently block shutdown or library replacement.

## Persistence and versioning

The session uses the literal `workspace` group in the store owned by `AppRuntimeOwner`.
GTK uses a per-library workspace location; TUI uses its selected configuration path.
Exact locations belong to the [managed file locations reference](../../reference/persistence/location.md).

The payload carries required `presentationVersion: 3`, kind `builtin` or `custom`, a matching preset key, and the base `sort`. A view may also carry `columnSort`, whose `column` is a track-field id. A custom-preset definition has no `columnSort`.
That marker covers nested presentation vocabulary only.
The payload still has no complete root schema version or resource budgets.
The [workspace session state reference](../../reference/workspace/session-state.md) owns the exact inventory and compatibility boundary.

## Implementation map

- [`WorkspaceService`](../../../app/include/ao/rt/WorkspaceService.h) captures, prepares, commits, and reports session operations.
- [`WorkspaceSnapshot`](../../../app/include/ao/rt/WorkspaceSnapshot.h) is the one live aggregate installed by restore.
- [`WorkspaceSessionState`](../../../app/include/ao/rt/WorkspaceSessionState.h) is the persistence candidate.
- [`WorkspaceSessionYamlSchema`](../../../app/runtime/WorkspaceSessionYamlSchema.h) owns explicit YAML mapping, the private strict document, and stable presentation conversion.
- [`ViewService`](../../../app/include/ao/rt/ViewService.h) creates and destroys candidate views.
- [`ConfigStore`](../../../app/include/ao/rt/ConfigStore.h) supplies explicit schema invocation, presence-aware candidate loading, and one-shot save.
- [`MainWindow.cpp`](../../../app/linux-gtk/app/MainWindow.cpp) owns current GTK restore/default/checkpoint sequencing.
- [`App.cpp`](../../../app/tui/App.cpp) owns TUI restore/checkpoint sequencing, and [`LibraryController.cpp`](../../../app/tui/LibraryController.cpp) owns exact active-view attachment and reload materialization.

## Test map

- [`WorkspaceSessionTest.cpp`](../../../test/runtime/WorkspaceSessionTest.cpp) proves missing-group no-op behavior, exact same-list view focus, one-event multi-view restore, initial history, index rejection, empty candidates, another library's session, candidate cleanup, and retention of a rejected workspace group across a fallback checkpoint.
- [`LibraryRestoreStateIdentityTest.cpp`](../../../test/runtime/LibraryRestoreStateIdentityTest.cpp) proves that a session saved before a library restore opens no view over another list.
- [`WorkspaceSessionYamlSchemaTest.cpp`](../../../test/runtime/WorkspaceSessionYamlSchemaTest.cpp) proves canonical stable vocabulary, semantic round trip, and invalid-document rejection.
- [`HeadlessShellTest.cpp`](../../../test/runtime/HeadlessShellTest.cpp) proves cross-runtime view and presentation reconstruction.
- [`WorkspaceHistoryTest.cpp`](../../../test/runtime/WorkspaceHistoryTest.cpp) protects the history seeded by restore.
- [`MainWindowTest.cpp`](../../../test/linux-gtk/app/MainWindowTest.cpp) protects GTK workspace/playback restore composition.
- [`MainWindowSessionPresentationTest.cpp`](../../../test/linux-gtk/app/MainWindowSessionPresentationTest.cpp) proves restored presentation preservation, initial history replay, and new-view default selection.
- [`LibraryControllerTest.cpp`](../../../test/tui/LibraryControllerTest.cpp) proves exact same-list filtered-view attachment, valid empty-view acceptance, restored custom-preset visibility, reload and chooser-highlight preservation, and list-deletion fallback.

## Related documents

- [Workspace architecture](README.md)
- [Workspace navigation](navigation.md)
- [Workspace session state](../../reference/workspace/session-state.md)
- [Persistence and managed-state architecture](../persistence/README.md)
- [Grouped configuration store](../persistence/config-store.md)
- [GTK active-library lifecycle](../frontend/gtk/active-library-lifecycle.md)
- [TUI interaction](../frontend/tui.md)
