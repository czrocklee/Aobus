---
id: presentation.volume-control
type: spec
status: current
domain: presentation
summary: Defines shared volume projection, scroll and mute policy, indicator and tooltip mapping, and GTK precision-control behavior.
---
# Volume-control specification

## Scope

This specification owns platform-neutral volume presentation and interaction policy plus the current GTK compact-control adapter.
It does not own backend volume application or persisted output selection; those belong to runtime playback.

## Code boundary

`VolumeViewModel` under `ao::uimodel` subscribes to `rt::PlaybackService`, derives semantic state, and sends volume/mute commands.
`VolumeControlWidget` maps the semantic indicator kind to a GTK symbolic icon and owns GTK gestures, icon button, popovers, timeout, and scale widgets.
The declarative shell component only constructs the widget and does not duplicate volume policy.

## Terminology

- **Explicit mute** is the runtime `muted` flag.
- **Zero-volume appearance** uses the muted icon while explicit mute may remain false.
- **Hardware-assisted** is runtime output state surfaced in the tooltip.
- The **scroll bubble** is the transient percentage popover shown after wheel input.

## Invariants

- The control is visible only when runtime volume is available.
- Volume values sent by UIModel interaction helpers are clamped to `[0, 1]`.
- One wheel event changes level by two percentage points.
- Raising volume while explicitly muted clears explicit mute.
- Lowering volume to zero does not set explicit mute by itself.
- Muted or non-positive volume produces the `Muted` indicator kind; thresholds at 33% and 66% select `Low`, `Medium`, and `High`.
- GTK maps each indicator kind to its corresponding symbolic volume icon.
- Tooltip percentage is rounded to the nearest integer and appends `Muted` before `Hardware` context.
- UI rendering callbacks do not become volume state authority.

## State model

`VolumeViewState` contains visibility, normalized level, hardware-assisted flag, explicit mute, semantic `VolumeIndicatorKind`, and tooltip.
The view model retains runtime subscriptions and one render callback.

GTK retains one icon button, precision popover, vertical scale, mute toggle, scroll bubble, and optional timeout connection.

## Commands and transitions

Runtime output-device, playback-start, volume, and mute observations refresh the complete view state.
Direct scale edits call `setVolume`; the mute toggle calls `setMuted`; middle click toggles explicit mute.

Primary click opens the precision popover above the button.
Its vertical scale places maximum at the top, ranges from zero to one, steps by 0.02, pages by 0.1, and does not draw GTK's own value text.
The explicit mute toggle sits below it.

Vertical wheel input applies the shared two-percent rule and shows the updated percentage in a non-targetable bubble when the precision popover is closed.
The previous bubble timeout is replaced; the bubble closes after 500 milliseconds.

## Failure and cancellation

Volume interaction has no separate recoverable error surface; runtime accepts or narrows device behavior through `PlaybackService`.
Widget destruction disconnects the timeout and unparents both popovers before their button parent disappears.
View-model subscriptions release with the model.

## Persistence and versioning

The control owns no serialized state.
Runtime volume/output state and application output preferences belong to playback and managed-state owners.

## Frontend observations

The GTK control is an icon-only, frameless button suitable for a compact shell slot.
Its percentage appears in the tooltip, precision popover, and scroll bubble.
`setOrientation` is a no-op for this control; shell placement does not change its internal vertical precision scale.

## Implementation map

- [`VolumeViewModel.cpp`](../../../app/uimodel/playback/output/VolumeViewModel.cpp) owns projection, semantic indicator/tooltip, scroll, and mute policy.
- [`VolumeControlWidget.cpp`](../../../app/linux-gtk/playback/VolumeControlWidget.cpp) owns GTK icon mapping, gestures, and popovers.
- [`VolumeControlComponent.cpp`](../../../app/linux-gtk/layout/component/playback/VolumeControlComponent.cpp) registers the shell component.

## Test map

- [`VolumeViewModelTest.cpp`](../../../test/unit/uimodel/playback/output/VolumeViewModelTest.cpp) protects shared semantic mapping and commands.
- [`VolumeControlWidgetTest.cpp`](../../../test/unit/linux-gtk/playback/VolumeControlWidgetTest.cpp) protects GTK state and symbolic-icon rendering.

## Related documents

- [Playback architecture](../../architecture/playback.md)
- [Presentation architecture](../../architecture/presentation.md)
- [Application shell architecture](../../architecture/application-shell.md)
- [Audio quality architecture](../../architecture/audio-quality.md)
