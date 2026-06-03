# Bottom Volume Control

Date: 2026-06-03
Status: Implemented

## Overview

The bottom bar's volume control component has been updated to use a compact, icon-centric design in the Modern layout, resolving the visual weight imbalance caused by the previous vertical segmented bar. 

## Default State

- Rendered as a compact icon-only button in the center control group of the Modern layout.
- Utilizes standard symbolic GTK speaker icons reflecting the active auditory state:
  - Muted or volume <= 0%: `audio-volume-muted-symbolic`
  - 1-33%: `audio-volume-low-symbolic`
  - 34-66%: `audio-volume-medium-symbolic`
  - 67-100%: `audio-volume-high-symbolic`
- Hover tooltip displays the percentage format: `Volume: 64%`, appending `(Muted)` or `(Hardware)` contexts when applicable.

## Interactions

### Scroll Feedback
- Hovering and scrolling adjusting the volume (2% step size per tick) without requiring a popup click.
- A transient popup bubble (the "scroll bubble") displays the new percentage for 500ms following any scroll action, fading automatically.
- Scrolling up while explicitly muted clears the mute state and restores audio output.
- Scrolling down to 0% sets the volume to 0 (resulting in a muted icon), but does not activate the explicit `muted` flag on the backend.

### Precision Popover
- A primary left-click triggers a precision popover positioned above the icon.
- This popover features a vertical `Gtk::Scale` slider.
- High volume is oriented at the top of the slider; 0% is at the bottom.
- An explicit "Mute" toggle button resides beneath the slider.
- `set_autohide(true)` dismisses the popover when the user clicks elsewhere.

### Shortcut Toggles
- A middle-click over the main volume icon button acts as a rapid shortcut to toggle explicit mute state.
