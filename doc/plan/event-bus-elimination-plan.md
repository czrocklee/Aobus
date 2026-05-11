# EventBus Elimination Plan

Date: 2026-05-11
Status: In Progress
Progress: Phase 1, 2, and 3 COMPLETED. Ready for Phase 4 (Purge).

## Goal

Remove the global `EventBus` from the public API. Event types are co-located with their owning service, and external consumers subscribe through service methods instead of a generic type-erased bus.

## Current State Summary

### Event → Publisher mapping (1:1, no exceptions)

| Event | Publisher |
|-------|-----------|
| `PlaybackTransportChanged` | PlaybackService |
| `NowPlayingTrackChanged` | PlaybackService |
| `PlaybackOutputChanged` | PlaybackService |
| `PlaybackStopped` | PlaybackService |
| `PlaybackDevicesChanged` | PlaybackService |
| `PlaybackQualityChanged` | PlaybackService |
| `RevealTrackRequested` | PlaybackService |
| `FocusedViewChanged` | WorkspaceService |
| `SessionRestored` | WorkspaceService |
| `ViewDestroyed` | ViewService |
| `ViewFilterChanged` | ViewService |
| `ViewSortChanged` | ViewService |
| `ViewGroupingChanged` | ViewService |
| `ViewSelectionChanged` | ViewService |
| `ViewListChanged` | ViewService |
| `TracksMutated` | LibraryMutationService |
| `ListsMutated` | LibraryMutationService |
| `LibraryImportCompleted` | LibraryMutationService |
| `ImportProgressUpdated` | LibraryMutationService |
| `NotificationPosted` | NotificationService |
| `NotificationDismissed` | NotificationService |

### Subscriber analysis

**UI consumers** (all go through `_session.events()`):

| File | Events subscribed | Target service |
|------|------------------|----------------|
| PlaybackBar.cpp:101 | PlaybackTransportChanged | PlaybackService |
| PlaybackBar.cpp:169 | PlaybackOutputChanged | PlaybackService |
| PlaybackBar.cpp:172 | PlaybackDevicesChanged | PlaybackService |
| PlaybackBar.cpp:175 | PlaybackQualityChanged | PlaybackService |
| StatusBar.cpp:198 | PlaybackTransportChanged | PlaybackService |
| StatusBar.cpp:201 | PlaybackOutputChanged | PlaybackService |
| StatusBar.cpp:204 | PlaybackQualityChanged | PlaybackService |
| StatusBar.cpp:207 | NotificationPosted | NotificationService |
| StatusBar.cpp:223 | ViewSelectionChanged | ViewService |
| StatusBar.cpp:227 | LibraryImportCompleted | LibraryMutationService |
| PlaybackController.cpp:122 | PlaybackTransportChanged | PlaybackService |
| PlaybackController.cpp:132 | PlaybackStopped | PlaybackService |
| TrackPageGraph.cpp:54 | RevealTrackRequested | PlaybackService |
| TrackPageGraph.cpp:85 | NowPlayingTrackChanged | PlaybackService |
| TrackPageGraph.cpp:129 | FocusedViewChanged | WorkspaceService |
| TrackPageGraph.cpp:132 | ViewDestroyed | WorkspaceService |
| MainWindow.cpp:317 | ImportProgressUpdated | LibraryMutationService |
| MainWindow.cpp:321 | LibraryImportCompleted | LibraryMutationService |
| MainWindow.cpp:332 | TracksMutated | LibraryMutationService |
| MainWindow.cpp:349 | ListsMutated | LibraryMutationService |

**Runtime cross-service consumers**:

| File | Events subscribed | Source service | Target dependency |
|------|------------------|----------------|-------------------|
| ListSourceStore.cpp:20 | ListsMutated | LibraryMutationService | (currently EventBus) |
| WorkspaceService.cpp:35 | ListsMutated | LibraryMutationService | (currently EventBus) |
| TrackDetailProjection.cpp:84 | FocusedViewChanged | WorkspaceService | (currently EventBus) |
| TrackDetailProjection.cpp:112 | ViewSelectionChanged | ViewService | (currently EventBus) |
| TrackDetailProjection.cpp:124 | TracksMutated | LibraryMutationService | (currently EventBus) |

## Architecture Principles

1. **UI subscribes via service methods.** `session.playback().onTransportChanged(cb)` is the API. No more `session.events()`.
2. **Events are co-located with their publisher.** Event structs live in the service header that publishes them.
3. **EventBus becomes a private runtime implementation detail.** It serves cross-service communication within `app/runtime` only. This keeps the complexity manageable for TrackDetailProjection and other runtime-to-runtime subscribers without requiring a reference chain explosion.
4. **No new abstractions.** Each service holds a `std::vector<std::move_only_function<...>>` per event type, exactly the same pattern EventBus already uses.

## Phase 1: Signal helper + Service subscribe methods

### 1a: Introduce a lightweight signal template in CorePrimitives.h

Replace the type-erased EventBus with a typed signal per event in each service:

```cpp
// CorePrimitives.h (new)
template <typename... Args>
class Signal final {
public:
  Subscription connect(std::move_only_function<void(Args...)> handler) {
    auto index = _handlers.size();
    _handlers.push_back(std::move(handler));
    return Subscription([this, index] { _handlers[index] = {}; });
  }

  void emit(Args... args) {
    for (auto& h : _handlers) {
      if (h) h(args...);
    }
  }

private:
  std::vector<std::move_only_function<void(Args...)>> _handlers;
};
```

This is simpler than EventBus (no type erasure, no type_index map). Each service owns one `Signal` per event type.

### 1b: Add events to PlaybackService

Move these structs into `PlaybackService.h` and add subscribe methods:

```cpp
class PlaybackService final {
public:
  // -- Event types --
  struct TransportChanged { ao::audio::Transport transport{}; };
  struct NowPlayingChanged { ao::TrackId trackId{}; ao::ListId sourceListId{}; };
  struct OutputChanged { OutputSelection selection{}; };
  struct DevicesChanged {};
  struct QualityChanged { ao::audio::Quality quality{}; bool ready{}; };
  struct RevealTrackRequested { ao::TrackId trackId{}; ao::ListId preferredListId{}; ViewId preferredViewId{}; };
  struct Stopped {};

  // -- Subscribe methods --
  Subscription onTransportChanged(std::move_only_function<void(TransportChanged const&)>);
  Subscription onNowPlayingChanged(std::move_only_function<void(NowPlayingChanged const&)>);
  Subscription onOutputChanged(std::move_only_function<void(OutputChanged const&)>);
  Subscription onDevicesChanged(std::move_only_function<void(DevicesChanged const&)>);
  Subscription onQualityChanged(std::move_only_function<void(QualityChanged const&)>);
  Subscription onRevealTrackRequested(std::move_only_function<void(RevealTrackRequested const&)>);
  Subscription onStopped(std::move_only_function<void(Stopped const&)>);

  // -- Commands (existing) --
  void play(...);
  // ...

private:
  Signal<TransportChanged> _transportChangedSignal;
  Signal<NowPlayingChanged> _nowPlayingChangedSignal;
  // ...
};
```

Implementation in `PlaybackService.cpp`:
- Move `events.publish(PlaybackTransportChanged{...})` → `_transportChangedSignal.emit(TransportChanged{...})`
- `onTransportChanged()` → `return _transportChangedSignal.connect(std::move(handler));`

### 1c: Same pattern for the other 4 services

**WorkspaceService**: Gains `FocusedViewChanged` + `SessionRestored` + subscribe methods.
**ViewService**: Gains `ViewDestroyed`, `ViewFilterChanged`, `ViewSortChanged`, `ViewGroupingChanged`, `ViewSelectionChanged`, `ViewListChanged` + subscribe methods.
**LibraryMutationService**: Gains `TracksMutated`, `ListsMutated`, `LibraryImportCompleted`, `ImportProgressUpdated` + subscribe methods.
**NotificationService**: Gains `NotificationPosted`, `NotificationDismissed` + subscribe methods.

## Phase 2: Migrate UI consumers

Each UI widget switches from `_session.events().subscribe<X>(...)` to `_session.service().onXxx(...)`.

### PlaybackBar (4 events → all PlaybackService)

Before:
```cpp
_transportChangedSub = _session.events().subscribe<PlaybackTransportChanged>([this](auto& ev) { ... });
_outputChangedSub   = _session.events().subscribe<PlaybackOutputChanged>([this](auto&) { rebuildOutputList(); });
_devicesChangedSub  = _session.events().subscribe<PlaybackDevicesChanged>([this](auto&) { rebuildOutputList(); });
_qualityChangedSub  = _session.events().subscribe<PlaybackQualityChanged>([this](auto& ev) { ... });
```

After:
```cpp
_transportChangedSub = _session.playback().onTransportChanged([this](auto& ev) { ... });
_outputChangedSub   = _session.playback().onOutputChanged([this](auto&) { rebuildOutputList(); });
_devicesChangedSub  = _session.playback().onDevicesChanged([this](auto&) { rebuildOutputList(); });
_qualityChangedSub  = _session.playback().onQualityChanged([this](auto& ev) { ... });
```

### StatusBar (6 events → 4 services)

Before:
```cpp
_transportChangedSub   = _session.events().subscribe<PlaybackTransportChanged>(...);
_outputChangedSub      = _session.events().subscribe<PlaybackOutputChanged>(...);
_qualityChangedSub     = _session.events().subscribe<PlaybackQualityChanged>(...);
_notificationPostedSub = _session.events().subscribe<NotificationPosted>(...);
_selectionChangedSub   = _session.events().subscribe<ViewSelectionChanged>(...);
_importCompletedSub    = _session.events().subscribe<LibraryImportCompleted>(...);
```

After:
```cpp
_transportChangedSub   = _session.playback().onTransportChanged(...);
_outputChangedSub      = _session.playback().onOutputChanged(...);
_qualityChangedSub     = _session.playback().onQualityChanged(...);
_notificationPostedSub = _session.notifications().onPosted(...);
_selectionChangedSub   = _session.views().onSelectionChanged(...);
_importCompletedSub    = _session.mutation().onImportCompleted(...);
```

### PlaybackController (2 events → PlaybackService)

Before:
```cpp
_transportSub = _session.events().subscribe<PlaybackTransportChanged>(...);
_stoppedSub   = _session.events().subscribe<PlaybackStopped>(...);
```

After:
```cpp
_transportSub = _session.playback().onTransportChanged(...);
_stoppedSub   = _session.playback().onStopped(...);
```

### TrackPageGraph (4 events → 2 services)

Before:
```cpp
_revealSub         = _session.events().subscribe<RevealTrackRequested>(...);
_nowPlayingSub     = _session.events().subscribe<NowPlayingTrackChanged>(...);
_focusSub          = _session.events().subscribe<FocusedViewChanged>(...);
_viewDestroyedSub  = _session.events().subscribe<ViewDestroyed>(...);
```

After:
```cpp
_revealSub         = _session.playback().onRevealTrackRequested(...);
_nowPlayingSub     = _session.playback().onNowPlayingChanged(...);
_focusSub          = _session.workspace().onFocusChanged(...);
_viewDestroyedSub  = _session.views().onDestroyed(...);
```

### MainWindow (4 events → LibraryMutationService)

Before:
```cpp
_importProgressSubscription  = _session.events().subscribe<ImportProgressUpdated>(...);
_importCompletedSubscription = _session.events().subscribe<LibraryImportCompleted>(...);
_tracksMutatedSubscription   = _session.events().subscribe<TracksMutated>(...);
_listsMutatedSubscription    = _session.events().subscribe<ListsMutated>(...);
```

After:
```cpp
_importProgressSubscription  = _session.mutation().onImportProgress(...);
_importCompletedSubscription = _session.mutation().onImportCompleted(...);
_tracksMutatedSubscription   = _session.mutation().onTracksMutated(...);
_listsMutatedSubscription    = _session.mutation().onListsMutated(...);
```

## Phase 3: Migrate runtime cross-service subscribers

These are the 5 remaining subscriptions after Phase 2. They happen between runtime components, not UI.

### Option A: Direct service references (cleaner, preferred)

**ListSourceStore** subscribes to `ListsMutated`. It currently takes `EventBus&`. Change to take `LibraryMutationService&`:

```cpp
// ListSourceStore.h
ListSourceStore(ao::library::MusicLibrary& library, LibraryMutationService& mutation);

// ListSourceStore.cpp
_listsMutatedSubscription = _mutation.onListsMutated([this](auto& ev) { ... });
```

**WorkspaceService** subscribes to `ListsMutated`. It already has access to other services. Add `LibraryMutationService&`:

```cpp
// WorkspaceService constructor
WorkspaceService(ViewService& views, PlaybackService& playback,
                 LibraryMutationService& mutation,
                 ao::library::MusicLibrary& library,
                 std::shared_ptr<ConfigStore> configStore);

// WorkspaceService.cpp
listsMutatedSub = mutation.onListsMutated([this](auto& ev) { ... });
```

**TrackDetailProjection** subscribes to `FocusedViewChanged`, `ViewSelectionChanged`, `TracksMutated`. This is constructed inside `ViewService::detailProjection()`. Currently:

```cpp
TrackDetailProjection(DetailTarget target, ViewService& views, EventBus& events, MusicLibrary& library);
```

After:
```cpp
TrackDetailProjection(DetailTarget target, ViewService& views, WorkspaceService& workspace,
                      LibraryMutationService& mutation, MusicLibrary& library);
```

`ViewService::detailProjection()` would need `WorkspaceService&` and `LibraryMutationService&` passed in, or `ViewService` itself would need to hold references to them.

**Trade-off**: This adds 2 constructor parameters to `ViewService` and changes `TrackDetailProjection`'s constructor. The dependency graph becomes fully explicit.

### Option B: Keep EventBus as private runtime implementation detail

EventBus stays but is only used within `app/runtime`. No UI code accesses it. `AppSession::events()` is removed.

- `ListSourceStore`, `WorkspaceService`, `TrackDetailProjection` continue to use EventBus internally.
- EventBus is passed between services at construction time inside `AppSession::Impl` only.

**Trade-off**: Less churn in the runtime layer, but EventBus still exists.

### Recommendation: Phase 3 → Option A

The dependency chain is already explicit in `AppSession::Impl` (services are constructed with references to each other). Adding one more reference (`LibraryMutationService&` to `WorkspaceService` and `ListSourceStore`) is a small, natural extension.

For `TrackDetailProjection`, the extra parameters are justified: the class already depends on those services' events, making it visible in the constructor is honest.

## Phase 4: Final Purge (Completed)
1.  [x] Remove `EventBus&` constructor parameter and member from all services.
2.  [x] Remove all `publish()` calls from runtime services.
3.  [x] Remove `EventBus.h` and `EventTypes.h`.
4.  [x] Remove `AppSession::events()` accessor.
5.  [x] Remove all redundant `#include` directives.
6.  [x] Verify build and unit tests.

## Files Touched (by phase)

### Phase 1
- `app/runtime/CorePrimitives.h` — add `Signal` template
- `app/runtime/PlaybackService.h` — add event types + subscribe methods
- `app/runtime/PlaybackService.cpp` — replace `events.publish()` with `_signal.emit()`
- `app/runtime/WorkspaceService.h` — add event types + subscribe methods
- `app/runtime/WorkspaceService.cpp` — replace `events.publish()` with `_signal.emit()`
- `app/runtime/ViewService.h` — add event types + subscribe methods
- `app/runtime/ViewService.cpp` — replace `events.publish()` with `_signal.emit()`
- `app/runtime/LibraryMutationService.h` — add event types + subscribe methods
- `app/runtime/LibraryMutationService.cpp` — replace `events.publish()` with `_signal.emit()`
- `app/runtime/NotificationService.h` — add event types + subscribe methods
- `app/runtime/NotificationService.cpp` — replace `events.publish()` with `_signal.emit()`

### Phase 2
- `app/linux-gtk/ui/PlaybackBar.cpp` — switch subscriptions to `session.playback().onXxx()`
- `app/linux-gtk/ui/StatusBar.cpp` — switch subscriptions to service methods
- `app/linux-gtk/ui/PlaybackController.cpp` — switch subscriptions to `session.playback().onXxx()`
- `app/linux-gtk/ui/TrackPageGraph.cpp` — switch subscriptions to service methods
- `app/linux-gtk/ui/MainWindow.cpp` — switch subscriptions to `session.mutation().onXxx()`

### Phase 3
- `app/runtime/AppSession.cpp` — update construction to pass service refs instead of EventBus
- `app/runtime/AppSession.h` — add LibraryMutationService& to accessors if needed
- `app/runtime/ListSourceStore.h` / `.cpp` — EventBus& → LibraryMutationService&
- `app/runtime/WorkspaceService.cpp` — EventBus& → LibraryMutationService& for ListsMutated
- `app/runtime/ViewService.cpp` — pass WorkspaceService& + LibraryMutationService& to TrackDetailProjection
- `app/runtime/TrackDetailProjection.h` / `.cpp` — EventBus& → WorkspaceService& + ViewService& + LibraryMutationService&

### Phase 4
- `app/runtime/EventBus.h` — delete
- `app/runtime/EventTypes.h` — delete
- `app/runtime/BusLog.h` — delete
- All files with `#include "EventBus.h"` or `#include "EventTypes.h"` — remove includes
- `app/runtime/AppSession.h` — remove `events()` method
- `app/runtime/AppSession.cpp` — remove `EventBus eventBus` member

## Risk Assessment

| Risk | Likelihood | Mitigation |
|------|-----------|------------|
| Circular dependency between services | Low | No circular event flow exists; publish is always one-directional |
| TrackDetailProjection constructor parameter explosion | Medium | Acceptable — 5 params is still manageable, and the class is internal |
| Build break between phases | Medium | Each phase is a self-contained, buildable commit |
| Signal re-entrancy (handler emits same signal) | Low | Same behavior as existing EventBus; handlers are appended, not replaced |

## Non-Goals

- Changing event payload structures (enriching with additional data)
- Splitting coarse events into fine-grained events (e.g., `PlaybackTransportChanged` → `Started`/`Paused`/`Stopped`)
- Changing the `Subscription` class
- Affecting the `TrackListProjection` subscription model (it has its own `subscribe` method, not EventBus-based)
