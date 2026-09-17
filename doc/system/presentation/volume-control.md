---
id: presentation.volume-control
---
# Volume-control specification

## Scope

This specification owns platform-neutral volume presentation and relative
interaction policy plus the current GTK compact-control adapter. It does not own
backend volume application or persisted output selection. Runtime owns current
playback state; UIModel restore policy and frontend lifecycle owners govern any
durable output preference.

## Code boundary

`VolumeViewModel` sends volume and mute commands. Its presenting form subscribes
to `rt::PlaybackService`, filters snapshots to volume-state changes, and derives
semantic `VolumeViewState`; its command-only form does not subscribe or render.

`VolumeControlWidget` maps the semantic indicator to GTK symbolic icons and owns
GTK gestures, button, popovers, timeout, and scale widgets. The declarative shell
component only constructs the widget.

## Presentation contract

`VolumeViewState` carries availability, level, hardware-assisted state, explicit
mute, semantic indicator kind, and localized tooltip.

- The control is visible only when runtime volume is available.
- Explicit mute is the runtime `muted` flag. A non-positive level uses the muted
  icon but does not itself set explicit mute.
- Muted or non-positive volume selects `Muted`; positive levels through 33% and
  66% select `Low` and `Medium`, and a higher level selects `High`.
- Tooltip percentage is the level multiplied by 100 and rounded to the nearest
  integer. Its selector gives explicit mute precedence over hardware-assisted
  context.
- Unrelated playback snapshots do not trigger a render. Runtime remains the
  state authority even when the callback renders a complete volume view.

The presenting constructor requires a callable render callback and performs an
initial projection. The single-argument constructor is command-only: it skips
catalog state, initial projection, subscription, and tooltip construction.

## Commands and transitions

`handleVolumeChanged` forwards an absolute level to runtime; it does not add a
second clamp. The GTK scale itself is bounded to `[0, 1]`.

The shared relative helpers (`handleScroll` and `adjustVolume`) clamp their
target to `[0, 1]`. Raising the target above the current level while explicitly
muted first clears mute; reducing the target, including to zero, leaves explicit
mute unchanged. One vertical wheel event changes the target by two percentage
points.

GTK primary click opens a popover above the button. Its vertical scale places
maximum at the top, ranges from zero to one, steps by 0.02, pages by 0.1, and
hides GTK's own value text. The explicit mute toggle sits below it; middle click
toggles explicit mute.

Wheel input displays the updated percentage in a non-targetable bubble when the
precision popover is closed. A new wheel event replaces the previous timeout,
and the bubble closes after 500 milliseconds.

## Lifetime and failure

Volume interaction has no separate recoverable error surface; commands cross
`PlaybackService`. The presenting model owns its snapshot subscription. GTK
widget destruction disconnects the bubble timeout and unparents both popovers
before their button parent disappears.

The control owns no serialized state. `setOrientation` is intentionally a no-op;
shell placement does not change the internal vertical scale.

## Evidence

- [`VolumeViewModel.h`](../../../app/include/ao/uimodel/playback/output/VolumeViewModel.h)
  defines command-only and presenting construction.
- [`VolumeViewModel.cpp`](../../../app/uimodel/playback/output/VolumeViewModel.cpp)
  owns snapshot filtering, semantic projection, relative clamping, and mute
  policy.
- [`PlaybackOutputText.cpp`](../../../app/uimodel/playback/output/PlaybackOutputText.cpp)
  owns the tooltip selector.
- [`VolumeControlWidget.cpp`](../../../app/linux-gtk/playback/VolumeControlWidget.cpp)
  owns GTK mapping, gestures, popovers, and teardown.
- [`VolumeViewModelTest.cpp`](../../../test/unit/uimodel/playback/output/VolumeViewModelTest.cpp)
  protects shared state and commands, including relative clamping and unrelated
  snapshot suppression.
- [`VolumeControlWidgetTest.cpp`](../../../test/unit/linux-gtk/playback/VolumeControlWidgetTest.cpp)
  protects GTK state and icon rendering.

## Related documents

- [Playback architecture](../playback/README.md)
- [Presentation architecture](README.md)
- [Application shell architecture](../shell/README.md)
