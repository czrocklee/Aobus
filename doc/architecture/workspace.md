---
id: architecture.workspace-lifecycle
type: architecture
status: current
domain: workspace
summary: Defines runtime view identity, workspace aggregate ownership, navigation, and semantic session boundaries.
---
# Workspace architecture

## Scope

This document owns the application-runtime graph for views and the workspace that contains them.
It defines runtime view identity, source and projection ownership, the open and focused view aggregate, navigation history, and the semantic state captured for workspace restoration.

It does not own frontend startup, active-library replacement, process shutdown, track-source membership, playback succession, persisted field schemas, or widget layout.
Those concerns belong to the interactive session lifecycle, library, playback, persistence, presentation, and reference owners linked below.

## System context

The [architecture landscape](README.md) classifies workspace as an application system.
The [system architecture](system-overview.md) places `ViewService` and `WorkspaceService` in application runtime above library sources and projections.

```text
Library sources and projections
  -> ViewService
       ViewId + source lease + projection + selection + presentation state
  -> WorkspaceService
       open views + focused view + semantic navigation + history + session candidate
  -> UIModel and frontend observers
```

The public boundary is under `app/include/ao/rt/`; implementations live under `app/runtime/`.
`AppRuntime` owns both services, but the [interactive session lifecycle architecture](interactive-session-lifecycle.md) owns construction and destruction of the containing runtime graph.

## Responsibilities

### View service

`ViewService` allocates runtime-local `ViewId` values and owns each view's content state, source lease, live projection, selection, presentation state, revision, and `Attached`, `Detached`, or `Destroyed` lifecycle.
A destroyed view releases its projection and source leases and cannot become usable again under the same identity.

`ViewId` is execution state rather than a durable library or navigation identity.
History and sessions therefore retain semantic reconstruction inputs instead of persisting a live handle.

### Workspace service

`WorkspaceService` is the canonical aggregate for open views and focus within one `AppRuntime`.
It owns semantic target resolution, browser-like navigation history, custom presentation presets associated with the workspace, and the workspace snapshot and restore boundary.

The service borrows `ViewService` to create, focus, replay, and destroy views.
It observes committed list deletion through `LibraryChanges` and closes views whose base list no longer exists.
Its narrow playback dependency is limited to workspace-originated reveal intent; transport and succession remain playback authorities.

### Presentation consumers

UIModel and frontends may retain a current `ViewId` as a live handle and project workspace observations.
They do not rebuild the authoritative aggregate, allocate independent workspace identities, or persist `ViewId` across runtime lifetimes.

## Boundaries and dependency direction

- `WorkspaceService` may coordinate `ViewService`, library-change observations, presentation values, and narrow playback reveal commands.
- `ViewService` may acquire library sources and construct projections, but it does not own focus, navigation history, frontend layout, or managed paths.
- The [library architecture](library.md) owns `ListId`, source membership, leases, and projection mechanics below the view boundary.
- The [track expression architecture](track-expression.md) owns expression compilation and membership semantics used by filtered and Smart List views.
- The [presentation architecture](presentation.md) owns display and interaction policy, not the workspace aggregate.
- The [persistence and managed-state architecture](persistence-and-managed-state.md) owns stores and codecs; workspace owns the meaning of its session candidate.
- The [playback architecture](playback.md) owns transport and succession even when a workspace command initiates reveal or play intent.
- CLI uses `CoreRuntime` and does not acquire view or workspace behavior.

## Data and control flow

### Navigation

```text
frontend or UIModel intent
  -> WorkspaceService resolves a semantic target
  -> ViewService reuses or creates a view and projection
  -> WorkspaceService commits the open/focused aggregate
  -> final semantic snapshot enters navigation history
  -> workspace and view observations reach presentation consumers
```

History stores list identity, filter text, and presentation state rather than `ViewId`.
Replay resolves a matching live view or creates a replacement when the original identity no longer exists.
The [workspace navigation specification](../spec/workspace/navigation.md) owns transition, rollback, traversal, and observation behavior.

### Session capture and restore

```text
workspace aggregate
  -> semantic view reconstruction records
  -> managed-state codec and store
  -> validated candidate views
  -> one restored open/focused aggregate
```

The session excludes projections, source leases, selections, widget state, and runtime identities.
The [workspace session specification](../spec/workspace/session.md) owns current capture, validation, fallback, and commit behavior; the [session-state reference](../reference/workspace/session-state.md) owns exact fields.

## Structural constraints

- One `WorkspaceService` is the canonical open and focused view aggregate for one `AppRuntime`.
- Every live workspace view belongs to the same runtime's `ViewService`.
- `ViewId` is valid only within its allocating `ViewService` lifetime.
- `ListId` is interpreted only within the active library bound to that runtime.
- Navigation history is runtime aid state and is not itself persisted workspace session state.
- Workspace restoration reconstructs semantic views instead of resurrecting runtime objects.
- Frontend observers cannot become a second source of workspace truth.

## Failure, cancellation, and lifetime boundaries

Workspace commands run on the callback executor selected by the frontend and expose no independent cancellation surface.
View construction prepares source and projection resources before publication; a failed navigation leaves the committed aggregate and history position unchanged according to the current navigation specification.

Restore treats a missing group as an empty first-run state.
Malformed data or an unresolvable view returns a recoverable error, and candidate views created before a later failure are destroyed before the error returns.

[RFC 0016](../rfc/0016-coherent-workspace-transactions.md) proposes validated semantic commands and one revisioned candidate commit.
[RFC 0017](../rfc/0017-versioned-workspace-session.md) proposes library binding, bounded validation, stable session-local identities, and a versioned candidate.
Neither proposal is current behavior.

## Implementation map

- [`ViewService`](../../app/include/ao/rt/ViewService.h), [`ViewState`](../../app/include/ao/rt/ViewState.h), and [`ViewService.cpp`](../../app/runtime/ViewService.cpp) own runtime views and their resources.
- [`WorkspaceService`](../../app/include/ao/rt/WorkspaceService.h), [`WorkspaceViewState`](../../app/include/ao/rt/WorkspaceViewState.h), and [`WorkspaceService.cpp`](../../app/runtime/WorkspaceService.cpp) own the aggregate.
- [`NavigationHistory`](../../app/include/ao/rt/NavigationHistory.h) owns bounded semantic navigation history.
- [`WorkspaceSessionState`](../../app/include/ao/rt/WorkspaceSessionState.h) defines the current persistence candidate model.

## Test map

- [`ViewServiceLifecycleTest.cpp`](../../test/unit/runtime/ViewServiceLifecycleTest.cpp) protects view allocation, lifecycle, and resource release.
- [`WorkspaceNavigationTest.cpp`](../../test/unit/runtime/WorkspaceNavigationTest.cpp), [`WorkspaceHistoryTest.cpp`](../../test/unit/runtime/WorkspaceHistoryTest.cpp), and [`NavigationHistoryTest.cpp`](../../test/unit/runtime/NavigationHistoryTest.cpp) protect navigation ownership and replay.
- [`WorkspaceSessionTest.cpp`](../../test/unit/runtime/WorkspaceSessionTest.cpp) and [`HeadlessShellTest.cpp`](../../test/unit/runtime/HeadlessShellTest.cpp) protect semantic reconstruction and fallback.
- [`LibraryControllerTest.cpp`](../../test/unit/tui/LibraryControllerTest.cpp) protects TUI use of the runtime workspace authority.

## Related documents

- [Architecture landscape](README.md)
- [System architecture](system-overview.md)
- [Interactive session lifecycle architecture](interactive-session-lifecycle.md)
- [Library architecture](library.md)
- [Track expression architecture](track-expression.md)
- [Presentation architecture](presentation.md)
- [Persistence and managed-state architecture](persistence-and-managed-state.md)
- [Workspace navigation specification](../spec/workspace/navigation.md)
- [Workspace session specification](../spec/workspace/session.md)
- [Workspace session state reference](../reference/workspace/session-state.md)
