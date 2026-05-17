# Linux GTK Control Merge Implementation Plan

## Purpose

This document defines a small internal cleanup plan for the Linux GTK layout controls. The goal is to merge duplicated layout-component wrapper code while preserving the public layout component surface used by default layouts, templates, saved YAML layouts, and the layout editor.

This is not a user-facing behavior change. Existing component type names must continue to instantiate the same widgets with the same behavior.

## Current Verified State

`PlaybackComponents.cpp` registers several component types that are separate public layout entries but internally differ only by constructor parameters:

- `playback.playPauseButton`
- `playback.stopButton`
- `playback.playButton`
- `playback.pauseButton`
- `playback.currentTitleLabel`
- `playback.currentArtistLabel`

The transport button components all wrap `TransportButton` and pass a `TransportButton::Action`, optional `playSelectionInFocusedView()` callback, `showLabel`, and `size`.

The current title and artist label components both wrap `NowPlayingFieldLabel` and pass a `NowPlayingFieldLabel::Field`.

The default layout and built-in templates reference the existing public component type strings directly. Those names are part of the layout compatibility surface and should not be removed or renamed in this cleanup.

## Merge Candidates

### Merge Playback Transport Wrappers

Replace the four transport wrapper classes with one internal `TransportButtonComponent` that accepts a `TransportButton::Action`.

Keep all four registry entries and descriptors unchanged:

- `playback.playPauseButton`
- `playback.stopButton`
- `playback.playButton`
- `playback.pauseButton`

Each entry should create the shared internal component with the matching action. Play and play/pause actions keep the `playSelectionInFocusedView()` callback. Pause and stop keep an empty callback.

### Merge Now-Playing Field Wrappers

Replace `CurrentTitleLabelComponent` and `CurrentArtistLabelComponent` with one internal field label wrapper that accepts a `NowPlayingFieldLabel::Field`.

Keep both registry entries and descriptors unchanged:

- `playback.currentTitleLabel`
- `playback.currentArtistLabel`

### Keep Composite Controls Separate

Do not merge these as part of this cleanup:

- `status.defaultBar`
- `app.workspaceWithInspector`
- `playback.defaultBar` and `playback.compactControls` templates

These encode layout composition and interaction behavior rather than simple duplicate wrappers. Changing them would mix a refactor with layout behavior decisions.

## Implementation Steps

1. In `PlaybackComponents.cpp`, introduce one internal `TransportButtonComponent` class with constructor parameters for `LayoutContext`, `LayoutNode`, and `TransportButton::Action`.
2. Preserve the current `showLabel` and `size` property reads inside the shared transport wrapper.
3. Add a small helper that returns the `playSelectionInFocusedView()` callback only for `Play` and `PlayPause`.
4. Update `createPlayPauseButton`, `createStopButton`, `createPlayButton`, and `createPauseButton` to instantiate the shared wrapper with the appropriate action.
5. Introduce one internal `NowPlayingFieldComponent` wrapper around `NowPlayingFieldLabel`.
6. Update `createCurrentTitleLabel` and `createCurrentArtistLabel` to instantiate the shared field wrapper with the appropriate field.
7. Leave registry descriptors, default layout nodes, built-in templates, YAML format, and layout editor behavior unchanged.

## Test Plan

Run the focused Linux GTK layout tests:

```bash
./build.sh debug
```

If a narrower local test command is preferred during iteration, run the built test binary after configuring through the normal debug build and focus on:

- `LayoutComponentsTest`
- `LayoutEditorTest`

Validate these scenarios:

- All existing playback component types still create widgets.
- `showLabel` and `size` continue to affect all transport buttons.
- Default layout creation still references the same playback component type strings.
- Built-in templates still contain `playback.compactControls` and `playback.defaultBar`.
- The layout editor descriptor list remains unchanged.

## Acceptance Criteria

- The cleanup reduces duplicated wrapper classes in `PlaybackComponents.cpp`.
- No public layout component type is renamed or removed.
- Existing layout component tests pass.
- No user-facing design documentation needs to change because behavior and layout schema remain stable.

## Assumptions

- Compatibility with saved layouts is more important than reducing the public component list.
- This plan only covers internal wrapper consolidation, not a redesign of the playback bar or status bar.
