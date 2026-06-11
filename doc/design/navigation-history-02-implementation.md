# Navigation History Implementation Plan

## Phase 1: Add Pure History

Create `app/include/ao/rt/NavigationHistory.h`.

Public declarations:

```cpp
#pragma once

#include "CorePrimitives.h"
#include "TrackPresentation.h"

#include <cstddef>
#include <deque>
#include <optional>
#include <string>

namespace ao::rt
{
  struct NavigationPoint final
  {
    ListId listId{};
    std::string filterExpression{};
    TrackPresentationSpec presentation{};

    bool operator==(NavigationPoint const&) const = default;
  };

  class NavigationHistory final
  {
  public:
    static constexpr std::size_t kDefaultMaxSize = 256;

    explicit NavigationHistory(std::size_t maxSize = kDefaultMaxSize);

    void commit(NavigationPoint point);
    std::optional<NavigationPoint> back();
    std::optional<NavigationPoint> forward();
    std::optional<NavigationPoint> current() const;

    bool canGoBack() const noexcept;
    bool canGoForward() const noexcept;
    std::size_t size() const noexcept;
    std::optional<std::size_t> currentIndex() const noexcept;

  private:
    std::deque<NavigationPoint> _points;
    std::optional<std::size_t> _currentIndex;
    std::size_t _maxSize;
  };
}
```

Create `app/rt/NavigationHistory.cpp`.

Implementation rules:

- Clamp `maxSize` to at least `1`.
- Use `std::optional<std::size_t>` for empty history instead of sentinel index
  values.
- Deduplicate only against the current point, not against any earlier point.
- After front eviction, keep `_currentIndex` pointing to the same logical
  current point.
- `current()` returns a copy of the current point. The history size is capped at
  256 by default, so copy cost is acceptable and keeps the public API simple.

Add `rt/NavigationHistory.cpp` to `app/CMakeLists.txt`.

## Phase 2: Workspace State and Signals

Modify `WorkspaceService::Impl`:

```cpp
NavigationHistory navigationHistory;
bool replayingNavigation = false;
Signal<WorkspaceService::NavigationHistoryChanged const&>
  navigationHistoryChangedSignal;
```

Add a helper to snapshot the active view:

```cpp
std::optional<NavigationPoint> WorkspaceService::Impl::snapshotActiveView() const
{
  auto const viewId = layoutState.activeViewId;
  if (viewId == kInvalidViewId)
  {
    return std::nullopt;
  }

  auto const state = views.trackListState(viewId);
  if (state.lifecycle == ViewLifecycleState::Destroyed)
  {
    return std::nullopt;
  }

  return NavigationPoint{
    .listId = state.listId,
    .filterExpression = state.filterExpression,
    .presentation = state.presentation,
  };
}
```

Add `commitActiveViewIfRequested(options)`:

```cpp
void WorkspaceService::Impl::commitActiveViewIfRequested(
  NavigationOptions const options)
{
  if (!options.recordHistory || replayingNavigation)
  {
    return;
  }

  if (auto point = snapshotActiveView())
  {
    auto const beforeBack = navigationHistory.canGoBack();
    auto const beforeForward = navigationHistory.canGoForward();
    auto const beforeSize = navigationHistory.size();
    auto const beforeIndex = navigationHistory.currentIndex();

    navigationHistory.commit(std::move(*point));

    if (beforeBack != navigationHistory.canGoBack() ||
        beforeForward != navigationHistory.canGoForward() ||
        beforeSize != navigationHistory.size() ||
        beforeIndex != navigationHistory.currentIndex())
    {
      emitNavigationHistoryChanged();
    }
  }
}
```

If this diff check feels too broad during implementation, emit after every
successful non-replaying commit. The API contract does not require signal
coalescing.

## Phase 3: Navigation Command Recording

Update `WorkspaceService::navigateTo`:

```cpp
void WorkspaceService::navigateTo(NavigationTarget const& target,
                                  NavigationOptions options)
{
  auto targetViewId = resolveOrCreateTargetView(target);
  if (targetViewId == kInvalidViewId)
  {
    return;
  }

  focusView(targetViewId);
  _impl->commitActiveViewIfRequested(options);
}
```

Keep the current public behavior:

- `ListId`: reuse an unfiltered view with the same list id when available.
- `std::string`: create an ad-hoc filtered view.
- `GlobalViewKind::AllTracks`: resolve to `kAllTracksListId`.

Avoid recursive `navigateTo` for `GlobalViewKind::AllTracks`; resolve it in the
same function so one command maps to one possible commit.

## Phase 4: Presentation Command

Add workspace presentation methods:

```cpp
void WorkspaceService::setActivePresentation(
  TrackPresentationSpec const& presentation,
  NavigationOptions options)
{
  auto const viewId = _impl->layoutState.activeViewId;
  if (viewId == kInvalidViewId)
  {
    return;
  }

  _impl->views.setPresentation(viewId, presentation);
  _impl->commitActiveViewIfRequested(options);
}
```

For id lookup, `WorkspaceService` must resolve both built-in and custom
presentations:

```cpp
std::optional<TrackPresentationSpec>
WorkspaceService::Impl::presentationForId(std::string_view id) const;
```

Resolution order:

1. Built-in preset from `builtinTrackPresentationPreset(id)`.
2. Custom preset whose `preset.spec.id == id`.
3. `std::nullopt`.

Do not default unknown ids to `songs` at the workspace boundary. A missing
custom preset should not create a misleading history entry.

## Phase 5: Back and Forward

Implement:

```cpp
bool WorkspaceService::goBack()
{
  auto point = _impl->navigationHistory.back();
  if (!point)
  {
    return false;
  }

  auto replay = ReplayScope{_impl->replayingNavigation};
  _impl->restoreNavigationPoint(*point);
  _impl->emitNavigationHistoryChanged();
  return true;
}
```

`goForward()` is symmetric.

`restoreNavigationPoint(point)`:

- Find an existing non-destroyed track-list view matching `listId` and
  `filterExpression`.
- Create one if missing.
- Ensure the view is in `layoutState.openViews`.
- Set `layoutState.activeViewId`.
- Increment `layoutState.revision`.
- Emit `focusedViewChangedSignal`.
- Apply `point.presentation` with `ViewService::setPresentation`.

Do not call public `navigateTo()` from restore. Restore has different reuse
rules for filtered views and must not commit history.

## Phase 6: GTK Entry Points

`TrackPresentationButton::onPresentationSelected`:

- Keep the label update.
- Replace `_runtime.views().setPresentation(viewId, spec)` with
  `_runtime.workspace().setActivePresentation(spec)`.
- Capture only the spec in the idle callback. The workspace command should
  operate on the active view at execution time unless UX requires locking to
  the original view. If locking is required, add an overload that accepts
  `ViewId`; do not call `ViewService` directly from GTK.

`PlaybackComponents.cpp` album jump:

- Replace the local sequence with `_runtime.workspace().jumpToAlbum(_currentTrackId)`.

`MainWindow.cpp`:

- Add a `Gtk::GestureClick` controller.
- Check `gesture->get_current_button()`.
- Button `8`: call `_runtime.workspace().goBack()`.
- Button `9`: call `_runtime.workspace().goForward()`.
- Claim the sequence only when the command returns true.

## Phase 7: Session Restore

Session restore must not seed navigation history.

Current restore creates views directly and sets focus, so it should remain
history-neutral. If implementation later routes restore through public
workspace commands, pass `NavigationOptions{.recordHistory = false}`.

## Phase 8: Documentation and Build

- Update this design series if the final API differs.
- Add implementation source to `app/CMakeLists.txt`.
- Add tests to `test/CMakeLists.txt` if a new test file is created.
- Run:

```bash
./ao check
./ao tidy
```

For targeted iteration, preserve `/tmp/build/...` and run the failing test
binary directly from the existing build tree.
