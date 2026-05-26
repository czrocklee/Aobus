# Navigation History Model and API

## Snapshot Model

History stores a restoreable snapshot of the active track-list view:

```cpp
struct NavigationPoint final
{
  ListId listId{};
  std::string filterExpression{};
  TrackPresentationSpec presentation{};

  bool operator==(NavigationPoint const&) const = default;
};
```

`TrackPresentationSpec` is stored by value. The history must not depend only on
`presentation.id`, because custom presets can be renamed, modified, or removed
after the history point is committed. A history point should restore what the
user saw at commit time, not reinterpret a preset id through current preset
registry state.

The point does not store:

- `ViewId`: views are runtime objects and may be closed or recreated.
- Selection: selection is transient editing state, not navigation.
- Scroll position: not currently modeled by runtime services.
- Reveal target: reveal is an action after navigation, not part of the
  restoreable view state.

## History Data Structure

```cpp
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
```

`commit()` behavior:

1. If there is no current index, append the point and set index `0`.
2. If the new point equals the current point, no-op.
3. If current index is not the last point, erase all points after the current
   index.
4. Append the new point and move current index to the last point.
5. If the size exceeds max size, pop from the front and decrement current
   index.

`back()` and `forward()` only move the index and return the destination point.
They do not mutate views.

`current()` is a read-only helper for tests, diagnostics, and simple UI state.
It returns `std::nullopt` when the history is empty.

## Workspace API

`WorkspaceService` owns history and exposes user-level navigation commands:

```cpp
struct NavigationOptions final
{
  bool recordHistory = true;
};

using NavigationTarget = std::variant<ListId, std::string, GlobalViewKind>;

void navigateTo(NavigationTarget const& target,
                NavigationOptions options = {});

void setActivePresentation(TrackPresentationSpec const& presentation,
                           NavigationOptions options = {});

TrackPresentationSpec setActivePresentation(std::string_view presentationId,
                                             NavigationOptions options = {});

void jumpToAlbum(TrackId trackId);

bool goBack();
bool goForward();
bool canGoBack() const noexcept;
bool canGoForward() const noexcept;

struct NavigationHistoryChanged final
{
  bool canGoBack = false;
  bool canGoForward = false;
};

Subscription onNavigationHistoryChanged(
  std::move_only_function<void(NavigationHistoryChanged const&)> handler);
```

Existing `navigateTo(...)` call sites remain source-compatible because the
second parameter has a default.

## Private Workspace Helpers

Recommended private helpers in `WorkspaceService::Impl` or anonymous
functions:

```cpp
std::optional<NavigationPoint> snapshotActiveView() const;
void commitActiveViewIfRequested(NavigationOptions options);
void restoreNavigationPoint(NavigationPoint const& point);
void emitNavigationHistoryChanged();
```

Replay guard:

```cpp
class ReplayScope final
{
public:
  explicit ReplayScope(bool& replaying) : _replaying{replaying}
  {
    _previous = _replaying;
    _replaying = true;
  }

  ~ReplayScope() { _replaying = _previous; }

private:
  bool& _replaying;
  bool _previous = false;
};
```

`commitActiveViewIfRequested()` returns immediately when `options.recordHistory`
is false or `_replaying` is true.

## Restore Semantics

Restoring a point should prefer reusing an existing compatible view:

1. Find an existing track-list view whose `listId` and `filterExpression`
   match the point.
2. If found, focus it.
3. If not found, create a new attached track-list view with the point's
   `listId`, `filterExpression`, and presentation.
4. Apply `point.presentation` to the active view.

The existing `navigateTo(ListId)` behavior reuses only unfiltered views. The
restore helper should be explicit so filtered history points do not create a
new duplicate every time the user presses back or forward.

## Command Semantics

`navigateTo(target, options)`:

- Resolve or create the destination view.
- Focus the destination view.
- Commit the active view after the mutation if requested.

`setActivePresentation(presentation, options)`:

- No-op if no active view exists.
- Apply the full `TrackPresentationSpec` through `ViewService`.
- Commit the active view after the mutation if requested.

`jumpToAlbum(trackId)`:

- Navigate to `GlobalViewKind::AllTracks` with `recordHistory = false`.
- Apply the album presentation with `recordHistory = false`.
- Reveal `trackId`.
- Commit the final active view once.

This gives one back step from the album jump to the previous full view state.
