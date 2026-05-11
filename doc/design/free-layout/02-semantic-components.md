# Phase 2: Semantic Component Extraction

## Goal

Extract the current fixed UI into independently instantiable Aobus semantic components. The layout runtime should place these components freely, but each component should own its GTK internals and should communicate with runtime services through existing typed APIs.

This phase is intentionally not about exposing `Gtk::Button`, `Gtk::ToggleButton`, or generic signal/property bindings as user-facing layout leaves.

## Component Taxonomy

### Playback Components

Current source: `PlaybackBar`, `VolumeBar`, `OutputMenuModel`, `AobusSoul`, `AobusSoulWindow`.

Target components:

```text
playback.outputButton
playback.previousButton
playback.playButton
playback.pauseButton
playback.playPauseButton
playback.stopButton
playback.nextButton
playback.seekSlider
playback.timeLabel
playback.volumeControl
playback.qualityIndicator
playback.currentTitleLabel
playback.currentArtistLabel
```

First extraction should prioritize controls already present in `PlaybackBar`:

1. `playback.playPauseButton`
2. `playback.stopButton`
3. `playback.seekSlider`
4. `playback.timeLabel`
5. `playback.volumeControl`
6. `playback.outputButton`

`previousButton` and `nextButton` should wait until playback sequencing supports those commands cleanly.

### Library Components

Current source: `ListSidebarController`, `ListTreeNode`, `SmartListDialog`, `QueryExpressionBox`.

Target components:

```text
library.listTree
library.createSmartListButton
library.importButton
library.openLibraryButton
```

`library.listTree` can initially wrap the existing `ListSidebarController::widget()` and action setup. Later it should split visual tree, model, and controller.

### Track Components

Current source: `TrackViewPage`, `TrackPageGraph`, `TrackListAdapter`, `TrackRowDataProvider`, `TrackPresentation`.

Target components:

```text
tracks.table
tracks.filterEntry
tracks.groupSelector
tracks.columnChooserButton
tracks.selectionSummaryLabel
```

For Phase 2, `tracks.table` may wrap the existing `TrackViewPage` as a single semantic component. Fine-grained extraction of filter/group/column controls can happen after `MainWindow` uses layout documents.

### Inspector Components

Current source: `InspectorSidebar`, `CoverArtWidget`, `CoverArtCache`, `TagEditor`.

Target components:

```text
inspector.coverArt
inspector.trackDetails
inspector.metadataFields
inspector.audioProperties
inspector.tagEditor
```

`inspector.coverArt` can reuse `CoverArtWidget`, which already binds to an `ITrackDetailProjection`.

### Status Components

Current source: `StatusBar`, `NotificationService`, `LibraryMutationService`, `PlaybackService`, `ViewService`.

Target components:

```text
status.messageLabel
status.libraryTrackCount
status.selectionSummary
status.nowPlayingLabel
status.importProgress
status.playbackQuality
```

Phase 2 may keep `status.defaultBar` as a wrapper and extract individual labels later.

## Typed Runtime Binding Pattern

Each semantic component owns its runtime subscriptions. Example pattern:

```cpp
class PlaybackStopButtonComponent final : public ILayoutComponent
{
public:
  explicit PlaybackStopButtonComponent(ComponentContext& ctx, LayoutNode const& node)
    : _session{ctx.session}
  {
    _button.set_icon_name("media-playback-stop-symbolic");
    _button.signal_clicked().connect([this] { _session.playback().stop(); });

    _startedSub = _session.playback().onStarted([this] { refresh(); });
    _pausedSub = _session.playback().onPaused([this] { refresh(); });
    _idleSub = _session.playback().onIdle([this] { refresh(); });
    _stoppedSub = _session.playback().onStopped([this] { refresh(); });

    refresh();
  }

  Gtk::Widget& widget() override { return _button; }

private:
  void refresh();

  ao::rt::AppSession& _session;
  Gtk::Button _button;
  ao::rt::Subscription _startedSub;
  ao::rt::Subscription _pausedSub;
  ao::rt::Subscription _idleSub;
  ao::rt::Subscription _stoppedSub;
};
```

The component reads typed props from `node.props`, but behavior remains C++ and typed.

## Playback UI State Gap

The current `PlaybackService` has enough events for transport buttons, output changes, and quality changes. It is less complete for independent seek/time/volume components:

- `seek()` updates state but emits no position event.
- `setVolume()` and `setMuted()` update state but emit no volume event.
- The current `PlaybackBar` uses a tick callback for smooth seek/time display.

Recommended Phase 2 addition:

```text
PlaybackUiProjection
  input: PlaybackService events + frame clock/timer
  output: typed playback UI snapshot subscription
```

This projection should be shared through `ComponentContext`, not created separately by every playback component. `playback.seekSlider`, `playback.timeLabel`, `playback.playPauseButton`, and title/artist labels should subscribe to the same projection instance so the layout does not create duplicate frame-clock callbacks or timers.

Example snapshot:

```cpp
struct PlaybackUiSnapshot final
{
  ao::audio::Transport transport = ao::audio::Transport::Idle;
  bool ready = false;
  bool canPlay = false;
  bool canPause = false;
  bool canStop = false;
  bool canSeek = false;
  std::uint32_t positionMs = 0;
  std::uint32_t durationMs = 0;
  float volume = 1.0f;
  bool muted = false;
  bool volumeAvailable = false;
  std::string timeText;
  std::string title;
  std::string artist;
  std::uint64_t revision = 0;
};
```

This projection is not a generic binding engine. It is a typed UI model for playback components.

Canonical context field used by playback components:

```cpp
struct ComponentContext final
{
  ao::rt::AppSession& session;
  Gtk::Window& parentWindow;
  PlaybackUiProjection* playbackUi = nullptr;
};
```

This snippet shows only the fields relevant to playback. The overview contains the merged context definition. `PlaybackUiProjection` should be owned by `MainWindow` or a long-lived playback UI system, because semantic playback components may be destroyed and recreated when the layout changes.

## Existing Component Wiring Notes

### `library.listTree`

`ListSidebarController` currently requires callbacks at construction. The semantic component or adapter should wire those callbacks internally from `ComponentContext`:

- list selection -> `ctx.session.workspace().navigateTo(listId)`
- smart-list CRUD -> existing `ListSidebarController` methods and dialogs
- optional membership lookup -> keep the current `nullptr` behavior unless a later component needs direct membership access

The component does not need separate `MusicLibrary&`, `ListSourceStore&`, or `WorkspaceService&` fields because `AppSession&` already exposes those services.

### `inspector.coverArt`

`CoverArtWidget` already knows how to bind to an `ITrackDetailProjection`. The semantic component should create the focused-view projection through `ctx.session.views().detailProjection(ao::rt::FocusedViewTarget{})` unless a prop requests another target.

### `tracks.table`

`TrackRowDataProvider` stays long-lived outside layout components. A track component that wraps `TrackViewPage` or creates a `TrackListAdapter` must require `ctx.rowDataProvider != nullptr` and render an error component if the session has not been initialized yet.

## Actions and Accelerators

Visible semantic buttons should not be responsible for registering global actions or keyboard shortcuts. Actions and accelerators must remain registered by stable systems such as `MainWindow`, `Gtk::Application`, or dedicated action registrars, independent of the active layout.

This means a user can remove `playback.playPauseButton` from the layout and still use the configured play/pause shortcut. The button component may call the same runtime method as the action handler, but it must not be the only owner of the action.

## YAML Examples

### Custom Playback Row

```yaml
type: box
props:
  orientation: horizontal
  spacing: 4
children:
  - type: playback.outputButton
  - type: playback.playPauseButton
    props:
      showLabel: false
      size: large
  - type: playback.stopButton
  - type: playback.seekSlider
    layout:
      hexpand: true
  - type: playback.timeLabel
    props:
      format: elapsed-duration
  - type: playback.volumeControl
```

### Minimal Listening Layout

```yaml
type: box
props:
  orientation: vertical
  spacing: 8
children:
  - type: inspector.coverArt
    layout:
      minHeight: 300
  - type: playback.currentTitleLabel
  - type: playback.currentArtistLabel
  - type: playback.seekSlider
  - type: box
    props:
      orientation: horizontal
      spacing: 4
    children:
      - type: playback.playPauseButton
      - type: playback.stopButton
      - type: playback.volumeControl
```

## Migration Order

1. Add component wrappers that use existing widgets with minimal behavior changes.
2. Extract playback controls from `PlaybackBar` one at a time.
3. Add `PlaybackUiProjection` only when `seekSlider` and `timeLabel` become independent.
4. Keep `TrackPageGraph` as the page lifecycle manager; register `tracks.workspaceStack` or `tracks.table` only after the layout host can own the stack position.
5. Split `ListSidebarController` into visual/model/controller only after `library.listTree` can be instantiated by the registry.

## Tests

- Component factory registration for every semantic type.
- Playback button state transitions using a test or fake playback service if available.
- YAML snippets instantiate without unknown component errors.
- Existing playback/list/track behavior remains covered by current integration tests.

## Completion Criteria

- Default playback bar can be expressed as YAML using semantic playback leaf nodes.
- At least one non-default playback arrangement works without changing C++ layout code.
- Components keep typed `Subscription` members and do not introduce generic string-based signal dispatch.
- Existing fixed `PlaybackBar` can either be removed or kept only as a compatibility/default composite wrapper.
