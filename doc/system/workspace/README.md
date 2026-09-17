---
id: architecture.workspace-lifecycle
---
# Workspace architecture

## Find the contract for your change

- [View navigation and history](navigation.md)
- [Session capture and restore](session.md)
- [Stored workspace fields](../../reference/workspace/session-state.md)
- [Whole-runtime lifecycle](../session-lifecycle.md)

## Scope

This page explains the ownership model for runtime views and the workspace that
contains them. It is an introduction and routing page, not an inventory of every
workspace command, test, or persisted field.

The workspace owns open views, focus, custom presentation presets, semantic
navigation history, and session capture. It does not own track membership,
projection rendering, playback succession, frontend layout, library replacement,
or serialized field syntax.

## Ownership model

```text
library sources
  -> ViewService
       runtime-local ViewId + source lease + projection + selection + presentation
  -> WorkspaceService
       ordered open views + focus + presets + revision + semantic history
  -> UIModel and frontend consumers
```

`AppRuntime` owns one `ViewService` and one `WorkspaceService`. `ViewService`
owns each live view and the resources needed to produce it. `WorkspaceService`
is the only full-runtime authority that creates or destroys those views, decides
whether navigation reuses a plain view or creates a new one, and commits the
open/focused aggregate.

A `ViewId` is valid only for the lifetime of the `ViewService` that allocated it.
It is neither a durable identity nor a substitute for `ListId`. History and
sessions therefore retain semantic reconstruction inputs: list id, filter, and
presentation.

The workspace borrows `ViewService`, the runtime callback executor, and committed
library-change publication. It has no playback dependency. The narrow
`AppRuntime::jumpToAlbum()` composition navigates first and then submits playback
reveal without moving transport or succession ownership into workspace.

## Commit and observation contract

Workspace reads and mutations are confined to the runtime callback executor.
An accepted state-changing command installs one complete `WorkspaceSnapshot`,
increments its workspace-local revision once, and queues one `WorkspaceChanged`
event containing the committed snapshot. A successful no-op changes no revision
and publishes nothing.

View creation and presentation updates finish before the aggregate commit. A
recoverable preparation failure leaves the committed snapshot and navigation
cursor unchanged. Observer delivery is deferred through the same executor; each
event owns its snapshot, so a reentrant command cannot mutate the event being
delivered. Queued delivery retains only weak signal state and becomes harmless
after service destruction. An observer exception enters the signal boundary's
fatal handling; it does not turn a completed command into a failure.

Committed list deletion closes every open view over the deleted list in one
workspace commit. Destroying a view releases its projection and source leases.

## Navigation and session boundaries

Navigation history is bounded, runtime-only aid state. It stores semantic points,
not live ids, selection, scroll position, or widgets. Replay may reuse a matching
live view or create a replacement. See [Workspace navigation](navigation.md) for
target resolution, presentation intent, rollback, and history traversal.

Session persistence captures ordered semantic views, exact focus by entry index,
and custom presets. Restore constructs all candidate views before publishing the
aggregate and destroys candidates if a later creation fails. It does not persist
history, runtime ids, projections, selections, or frontend state. See
[Workspace session](session.md) for lifecycle behavior and
[Workspace session state](../../reference/workspace/session-state.md) for the
strict YAML surface.

## Dependency boundaries

- The [library architecture](../library/structure.md) owns `ListId`, source
  membership, leases, and projection mechanics.
- The [track expression architecture](../query/README.md) owns filter compilation
  and membership semantics.
- The [presentation architecture](../presentation/README.md) owns display and
  interaction policy.
- The [persistence architecture](../persistence/README.md) owns stores and
  managed files; workspace owns the meaning and installation of its candidate.
- The [playback architecture](../playback/README.md) owns transport and succession.
- Frontends may retain a current `ViewId` as a live handle, but cannot reconstruct
  a second authoritative workspace aggregate or persist that id.
- CLI uses `CoreRuntime` and does not acquire workspace behavior.

## Implementation authority

The public boundary is
[`app/include/ao/rt/`](../../../app/include/ao/rt/):
[`ViewService.h`](../../../app/include/ao/rt/ViewService.h),
[`WorkspaceService.h`](../../../app/include/ao/rt/WorkspaceService.h),
[`WorkspaceSnapshot.h`](../../../app/include/ao/rt/WorkspaceSnapshot.h), and
[`NavigationHistory.h`](../../../app/include/ao/rt/NavigationHistory.h).
Implementations are in [`app/runtime/`](../../../app/runtime/), principally
[`ViewService.cpp`](../../../app/runtime/ViewService.cpp),
[`WorkspaceService.cpp`](../../../app/runtime/WorkspaceService.cpp), and
[`NavigationHistory.cpp`](../../../app/runtime/NavigationHistory.cpp).

## Related documents

- [Architecture landscape](../README.md)
- [System architecture](../overview.md)
- [Interactive session lifecycle](../session-lifecycle.md)
- [Workspace navigation](navigation.md)
- [Workspace session](session.md)
- [Workspace session state](../../reference/workspace/session-state.md)
