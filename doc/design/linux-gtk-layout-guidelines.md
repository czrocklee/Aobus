# Linux GTK Layout Guidelines

This document defines the long-term layout rules for the Linux GTK frontend. It is intended to keep product UI predictable as components, the layout editor, and user CSS evolve.

## Core Principle

Layout ownership must be explicit:

```text
Parent layout owns relationship spacing.
Widget owns intrinsic visual size and behavior.
CSS owns skin, internal padding, and semantic region padding.
Custom drawing owns its real artwork bounds.
```

Do not solve layout by adding incidental margins to individual widgets. A widget should not know how far it should be from an unrelated sibling.

## Ownership Model

| Concern | Owner | Examples |
| --- | --- | --- |
| Distance between sibling controls | Parent container | `Gtk::Box::set_spacing()`, layout document `spacing` |
| Window-edge or region-edge inset | Semantic parent CSS class | `.ao-playback-strip`, `.ao-status-region` |
| Stable visual size | Component | output button hit target, volume bar width, time label character width |
| Click/touch target larger than glyph | Component | 32 px button containing 24 px glyph |
| Text color, radius, background, internal padding | CSS | `.ao-playback-button`, `.ao-device-row` |
| Actual drawn bounds of vector/custom art | Custom widget | `AobusSoul` fits stroke bounds into allocation |

## Spacing Scale

Use the shared spacing scale unless a drawing calculation requires a non-layout value.

| Name | Current C++ token | Typical use |
| --- | --- | --- |
| `xs` | `kSpacingXSmall` / 2 px | Tiny optical separation |
| `sm` | `kSpacingSmall` / 4 px | Dense internal grouping |
| `md` | `kSpacingMedium` / 6 px | Compact toolbar relationships |
| `lg` | `kSpacingLarge` / 8 px | Default toolbar/list sibling spacing |
| `xl` | `kSpacingXLarge` / 12 px | Separation between larger groups |

Avoid arbitrary layout values such as 5, 7, 10, or 13. If a value is optical and component-specific, name it locally and explain why it is not a layout spacing token.

## Toolbar Structure

Toolbars should use groups plus one clearly expandable area.

```text
╭──────────────────────── toolbar region ────────────────────────╮
│ semantic padding                                                │
│ ╭──────────────╮ ╭────────────────────────╮ ╭──────────────╮   │
│ │ leading group│ │ flexible primary area  │ │ trailing grp │   │
│ │ fixed size   │ │ hexpand/fill           │ │ fixed size   │   │
│ ╰──────────────╯ ╰────────────────────────╯ ╰──────────────╯   │
╰────────────────────────────────────────────────────────────────╯
```

Rules:

- The flexible primary area is the only child that should consume extra width.
- Leading and trailing groups should keep natural/fixed width.
- Group-to-group spacing belongs to the toolbar parent.
- Spacing inside a group belongs to that group.
- Edge breathing room belongs to the toolbar region CSS class.

For the playback bar, the intended structure is:

```text
leading group:  output button + transport buttons
flexible area:  seek slider
trailing group: time label + volume bar
```

The seek slider should be the only horizontally expanding playback child. Time and volume controls should stay fixed/natural size.

## Component Sizing

### Hit Target vs Glyph

Interactive controls should distinguish hit target size from visual glyph size.

Recommended pattern:

```text
button allocation: 32 x 32
glyph/artwork:     20-24 x 20-24, centered
```

This keeps controls easy to click without making icons visually oversized.

### Time Labels

Time labels should use stable text metrics:

- Use tabular numbers.
- Prefer character-width sizing for formatted time text.
- Do not use external margins to separate time from seek or volume controls.
- Keep the fixed width no larger than the longest expected format requires.

For `00:00 / 00:00`, a width of 13 characters is the source of truth. Avoid adding a second, larger CSS `min-width` unless it is intentionally part of the visual design.

### Sliders and Seek Bars

Sliders should separate hit area from relationship spacing:

- Internal vertical padding is acceptable when it improves the pointer hit area.
- Horizontal margins should be avoided unless they are part of the control's own visual drawing.
- The distance from a seek bar to a time label should be controlled by the parent group spacing, not the seek widget.

### Volume Bars

Volume bars should expose a compact fixed visual width. If a larger hit area is needed, create a control wrapper that centers the visual bar inside a larger allocation.

Do not put right-edge breathing room inside the volume widget. The playback strip or trailing group owns that space.

## Custom Drawing Rules

Custom widgets must make measurement and drawing agree.

For every custom-drawn widget:

1. Define the artwork coordinate system.
2. Define the true artwork bounds, including stroke width, animation variance, glow, and rotation if relevant.
3. Make `measure_vfunc()` return a meaningful intrinsic size.
4. Make `snapshot_vfunc()` fit the true artwork bounds into the allocated size.
5. Do not assume `height` is the correct drawing scale unless the artwork's full outer bounds are exactly `[-0.5, 0.5]` in both axes.

This prevents clipping bugs where a widget reports 24 px but draws 30+ px because stroke width or animation extends outside the nominal reference box.

## CSS Responsibilities

CSS should style semantic classes, not patch layout mistakes.

Good CSS responsibilities:

- region padding: `.ao-playback-strip`, `.ao-status-region`;
- component skin: radius, background, text color;
- internal padding that belongs to a component's identity;
- typography, including tabular numbers.

Avoid using CSS for:

- arbitrary sibling margins in production UI;
- compensating for oversized custom drawing;
- making one control line up with an unrelated control by trial and error.

## Layout Document Responsibilities

The built-in layout document should express semantic structure, not pixel hacks.

Preferred layout document usage:

- `box` and `template` express grouping.
- `spacing` expresses sibling rhythm.
- `hexpand` is applied to the single flexible region.
- `halign` and `valign` express alignment inside the parent's allocation.
- `cssClasses` attaches semantic region/component classes.

Avoid in built-in layouts:

- per-widget margin hacks;
- multiple competing `hexpand` children in the same toolbar row;
- fixed widths on parent groups when natural sizing is sufficient;
- layout values that duplicate component intrinsic sizes.

## Layout Editor Boundary

The layout editor can expose flexibility, but product defaults should remain curated.

- Built-in product layouts should follow these guidelines strictly.
- User-authored layouts may use escape hatches such as explicit margins, but those should not become the default product style.
- Templates should represent stable product patterns such as playback bar, status bar, workspace, and sidebar composition.
- If a layout pattern has behavioral constraints, prefer a first-class component over a loose collection of boxes.

## Recommended Long-Term Direction

### Bars as Templates, Not Composite Components

Bars (playback, status, etc.) should be expressed as layout templates, not as C++ composite components. A template is a named, reusable child-composition defined in `getBuiltInTemplates()` and referenced by the default layout via `type = "template"`.

```text
template "playback.defaultBar"          template "status.defaultBar"
  box horizontal                          box horizontal .ao-status-bar
    playback.outputButton                   status.playbackDetails
    playback.playPauseButton                spacer hexpand
    playback.stopButton                     status.nowPlaying
    playback.seekSlider hexpand             spacer hexpand
    playback.timeLabel                      status.importProgress
    playback.volumeControl                  status.notification
                                            separator vertical
                                            status.trackCount
```

Why templates over composite components:

- **Single composition mechanism.** Templates use the same layout primitives (box, spacer, separator) as every other part of the UI. A composite C++ class would be a second, incompatible mechanism.
- **User-customizable without recompilation.** Users can edit saved YAML layouts to reorder, add, or remove bar children. A composite component locks composition inside C++.
- **Shell regions stay in the default layout.** `ao-playback-strip` and `ao-status-region` remain on the outer wrapper nodes in `createDefaultLayout()`. The template only owns the inner bar composition. This keeps region-level styling separate from child ordering.
- **Conventions are enforced by review and tests, not by code locks.** The layout guidelines, review checklist, and geometry regression tests catch violations (multiple hexpand children, missing spacers, etc.) without preventing intentional user customization.

A template does not need to enforce the bar contract programmatically — it just needs to express the canonical composition. The contract is enforced socially and mechanically through tests.

### Shared Metric Tokens

Create one source of truth for GTK layout metrics:

- toolbar height;
- toolbar edge padding;
- control hit target sizes;
- glyph sizes;
- group spacing;
- compact/dense/comfortable density variants.

C++ layout constants and CSS variables should be generated from or synchronized with the same token set. Avoid manually duplicating `24`, `32`, `8`, and `12` across unrelated files.

### Geometry/Visual Regression Tests

Important bars should have geometry or screenshot regression coverage for at least:

- narrow width;
- default width;
- wide width;
- idle playback state;
- active playback state.

The minimum useful assertions are:

- no custom art is clipped;
- only the intended child expands;
- fixed controls keep expected size;
- trailing padding remains visible;
- labels do not jitter when text changes.

## Review Checklist

Use this checklist before merging GTK layout changes:

- Is spacing owned by the nearest parent rather than by child margins?
- Is there exactly one expandable child in each toolbar row?
- Are hit target and glyph size intentionally separated?
- Does every custom-drawn widget fit its true artwork bounds into allocation?
- Are CSS classes semantic rather than one-off positional patches?
- Are time-like labels using stable tabular metrics?
- Does the layout still behave at narrow and wide window widths?
- Are tests or visual checks updated for user-facing layout behavior?
