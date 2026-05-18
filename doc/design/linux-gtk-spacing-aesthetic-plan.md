# Linux GTK Spacing Aesthetic Plan

## Goal

This document defines the visual spacing strategy for the Linux GTK UI. The immediate trigger is the recent migration from C++ `set_margin_*()` calls to CSS utility classes such as `ao-margin-x-medium`. The migration improved centralization, but it also introduced a new risk: visual rhythm can still drift if every component chooses its own external margin.

The target aesthetic is a dense, calm desktop music application:

- predictable alignment between toolbars, lists, status areas, and side panels;
- compact controls without cramped text;
- strong separation between layout rhythm and component styling;
- rare, meaningful exceptions instead of many local margin tweaks.

## Core Rule

External spacing belongs to the parent layout. Internal spacing belongs to the component.

In practice:

- A component should usually not set its own outer `margin`.
- A component may set its own `padding`, `min-height`, typography, color, border, and radius.
- A parent container decides the distance between siblings through `Gtk::Box::set_spacing()`, layout `spacing`, or a parent/section semantic CSS class.
- Utility margin classes are allowed as a layout-editor escape hatch, not as the primary styling language for product UI.

This keeps visual rhythm stable when a widget is reused in a different context.

## Spacing Scale

Use a small, explicit spacing scale. Avoid arbitrary values like 5, 7, 10, or 13 unless they are tied to a non-layout drawing calculation.

Recommended tokens:

| Token | px | Use |
| --- | ---: | --- |
| `none` | 0 | tightly grouped controls, inline editor reset |
| `2xs` | 2 | micro status text, hairline separation |
| `xs` | 4 | compact rows, separator breathing room |
| `sm` | 6 | default toolbar/list edge padding |
| `md` | 8 | common control groups, dialog row spacing |
| `lg` | 12 | panel internal padding, dialog content padding |
| `xl` | 16 | major section separation in forms |
| `2xl` | 24 | inspector section spacing, large content groups |

The current code uses `2 / 4 / 6 / 8 / 12 / 24` already, with a few outliers such as `10` and `5`. The proposed scale keeps the existing feel but gives each value a role.

## CSS Class Policy

### Component Classes

Component classes describe what the widget is:

```css
.ao-playback-button
.ao-device-row
.ao-sidebar-row
.ao-track-section-header
.ao-status-bar
.ao-inspector-sidebar
.ao-tag-chip
```

These classes may define internal presentation:

- `padding`
- `min-height`
- `font-size`
- `font-weight`
- `opacity`
- `border-radius`
- colors

They should avoid default external `margin` unless that margin is inseparable from the component identity.

### Layout Classes

Layout classes describe a parent region:

```css
.ao-playback-strip
.ao-track-controls-bar
.ao-status-region
.ao-dialog-content
.ao-inspector-content
.ao-presentation-menu
```

These classes are allowed to define edge padding and sibling rhythm for their children. In GTK, sibling gaps still usually need `Gtk::Box::set_spacing()` because GTK4 CSS does not support `gap` for `Gtk::Box`.

### Utility Classes

Classes like these should be treated as low-level utilities:

```css
.ao-margin-x-medium
.ao-margin-y-small
.ao-margin-top-xlarge
```

They are acceptable for:

- layout editor generated documents;
- temporary migration code;
- one-off glue where creating a semantic class would be worse;
- tests that assert layout prop behavior.

They should not be the default style mechanism in reusable widgets. Widespread utility margins make the C++ code decide visual relationships while pretending CSS owns spacing.

## Current Issues

### Double Ownership

[StatusComponents.cpp](/home/rocklee/dev/Aobus/app/linux-gtk/layout/components/StatusComponents.cpp) applies `_container.set_margin(4)` while `.ao-status-bar` in [app.css](/home/rocklee/dev/Aobus/app/linux-gtk/app.css) also sets `margin`. This is the clearest example of mixed ownership.

Recommendation: remove status bar outer margin from the component. Put the edge inset on the parent status region or layout document. Keep `.ao-status-bar` for min-height and internal padding only.

### Undefined Utility

[TrackViewPage.cpp](/home/rocklee/dev/Aobus/app/linux-gtk/track/TrackViewPage.cpp) uses `ao-margin-top-large`, but [app.css](/home/rocklee/dev/Aobus/app/linux-gtk/app.css) defines `ao-margin-top-small`, `ao-margin-top-medium`, and `ao-margin-top-xlarge`, not `ao-margin-top-large`.

Recommendation: replace the section header utilities with a semantic `.ao-track-section-header` class. Do not add another utility just to patch the symptom.

### Directionality Loss

Several utility classes use `margin-left` and `margin-right` even though the original C++ code often used `set_margin_start()` and `set_margin_end()`.

Recommendation: prefer GTK logical CSS properties (`margin-start`, `margin-end`) for semantic classes when supported. If compatibility is uncertain, keep start/end spacing in C++ for that specific case rather than converting it to physical left/right utility classes.

### Token Drift

The code currently mixes `4`, `5`, `6`, `8`, `10`, `12`, and `24`. Some values are reasonable, but their roles are not documented.

Recommendation: normalize layout values to the proposed scale. `10` should generally become `8` or `12`; `5` chip spacing should become `4` or `6`.

## Control Inventory And Recommendations

### Root Layout

| Area | Current State | Recommendation |
| --- | --- | --- |
| `app-root` | Vertical box with menu, playback row, main paned, status bar. | Root should own vertical region order only. It should not depend on child margins to create global rhythm. |
| `playback-row` | Layout document uses horizontal `spacing = 6`. | Add a semantic class such as `.ao-playback-strip` through layout `cssClasses`; keep child margins at zero. Use edge padding on the strip if the row needs distance from the window edge. |
| `main-paned` | Split owns workspace/sidebar proportions. | No cosmetic margin on split children. Use pane content classes for internal padding. |
| `status.defaultBar` | Child sets margin in both C++ and CSS. | Parent/status region owns outer inset; status bar owns height and internal padding. |

### Playback Controls

| Control | Current State | Recommendation |
| --- | --- | --- |
| `TransportButton` / `.ao-playback-button` | Button has semantic class; size classes still named `playback-button-small/large`. | Good direction. Keep outer margin zero. Define padding/min-size per size class. Rename size classes to `ao-playback-button-small/large` for consistency. |
| `SeekControl` / `.ao-seekbar` | CSS applies vertical padding. | Acceptable because this padding is part of the control's hit area. Do not add margin. |
| `TimeLabel` | No custom spacing. | Keep margin-free. Parent playback row spacing decides distance. |
| `VolumeControl` / `VolumeBar` | Fixed size request `32 x 24`; custom drawing has internal vertical padding. | Treat as a compact control. No outer margin. Consider semantic class only if the visual size needs theme customization. |
| `OutputSelector` button / `.ao-output-logo` | Semantic class exists, no margin. | Good. Define button padding/min-size in CSS if needed, not via surrounding margins. |
| `OutputSelector` popover rows / `.ao-device-row` | Row has internal padding `6 x 16`; row box spacing hardcoded `10`. | Keep row padding. Normalize horizontal child spacing to `8` or `12`. If it must stay distinct, define a named constant or class comment explaining icon/text rhythm. |
| `NowPlayingStatusLabel` / `.ao-nowplaying` | Padding and rounded hover style. | Good as a pill-like label. No outer margin. |
| `NowPlayingFieldLabel` | Semantic field classes exist. | Keep margin-free. Typography/color only. |
| `PlaybackDetailsWidget` | Container spacing `8`; uses `ao-margin-x-medium`. | Replace utility margin with `.ao-playback-details`. Prefer no outer margin unless status bar layout requires it; if it needs breathing room from neighbors, the status bar should provide spacing. |

### Status Area

| Control | Current State | Recommendation |
| --- | --- | --- |
| `DefaultStatusBarComponent` / `.ao-status-bar` | Has CSS margin plus C++ margin. | Remove component margin. Keep `min-height: 24px` and vertical padding. Parent layout should decide window-edge inset. |
| Separator inside status bar | Uses `ao-margin-x-medium`. | Replace with `.ao-status-separator` and use logical margin or parent spacing. |
| `StatusNotificationLabel`, `LibraryTrackCountLabel`, `SelectionInfoLabel` | Mostly text with dim style. | Keep margin-free. Status bar or local row spacing controls placement. |
| `ImportProgressIndicator` | Container spacing `12`, progress bar fixed width. | Good as internal composition. No external margin. |

### Track View

| Control | Current State | Recommendation |
| --- | --- | --- |
| `TrackViewPage` controls bar | C++ margins `4` on all sides and spacing `8`. | Add `.ao-track-controls-bar`; move edge padding to CSS or layout class. Keep `set_spacing(8)` because it is sibling gap. |
| Quick filter entry | Expands inside controls bar. | No margin on entry. Parent controls bar handles edge padding. |
| Presentation button | No special margin. | Keep margin-free. |
| Presentation popover menu | Separators use C++ top/bottom margin `4`. | Create `.ao-presentation-menu-separator` or use a local helper. This is internal menu rhythm, not component outer margin. |
| Group section header label | Uses margin utilities, one undefined. | Replace with `.ao-track-section-header` with top/bottom padding or margin. Prefer padding if the label owns its clickable/visual area; margin is acceptable if GTK list header gives no parent spacing hook. |
| Status message label | C++ margins `4/2`. | Replace with `.ao-track-status-message` or make it part of a `.ao-track-status-row`. |
| Track table rows/cells | Inline editor classes reset margins/padding. | Good. Editing overlays need tight alignment. Keep explicit zero margin/padding in semantic classes. |
| Tags cell | `.ao-track-tags-cell` currently empty. | Use this for cell-specific internal padding only if needed. Do not add row-level margin. |

### Sidebar And Smart Lists

| Control | Current State | Recommendation |
| --- | --- | --- |
| `ListSidebarPanel` row box | Uses `ao-margin-x-medium` and `ao-margin-y-small`. | Replace with `.ao-sidebar-row`; use padding rather than margin so rows keep a reliable hit area. |
| Sidebar filter label | Uses `ao-margin-start-medium`. | Replace with `.ao-sidebar-filter-label` and prefer `margin-start` if it is indentation; otherwise use parent row spacing. |
| `QueryExpressionBox` completion labels | Uses `ao-margin-x-large` and `ao-margin-y-small`. | Replace with `.ao-query-completion-row`; use padding so hover/selection background covers the full row. |
| `SmartListDialog` | Local constants: margin `12`, spacing `8`, button top margin `16`. | Acceptable as dialog-local layout, but should migrate to `.ao-dialog-content`, `.ao-dialog-panel`, and `.ao-dialog-actions`. Keep spacing in C++ boxes. |

### Inspector And Tags

| Control | Current State | Recommendation |
| --- | --- | --- |
| `TrackInspectorPanel` root / `.ao-inspector-sidebar` | Root semantic class exists. Content box sets margin `12`, section spacing `24`. | Good visual direction. Rename the content box with `.ao-inspector-content`; move margin to CSS as panel internal padding. Keep `set_spacing(24)` or map to `2xl`. |
| Hero section / `.ao-hero-section` | Semantic class exists; centered cover. | Keep section margin-free. Parent inspector content spacing separates it. |
| Metadata rows | Local vertical row spacing `4`; metadata box spacing `12`. | Good. Consider `.ao-metadata-row` if more styling appears. |
| Audio section | Spacing `4`; header bottom margin `4`. | Replace header margin with `.ao-section-header` padding/margin-bottom. Section spacing remains parent-owned. |
| `TagEditor` root | Self margin `12`, box spacing `10`, chip spacing `5`. | In inspector context, parent should own edge padding. Remove root margin or convert to `.ao-tag-editor` internal padding only when used standalone. Normalize spacing to `8` or `12`; chip gap to `4` or `6`. |
| Tag chips / `.ao-tag-chip` | Padding `4 x 10`, rounded pill. | Keep as internal component style. Normalize horizontal padding to `8` or `12` if matching the token scale matters more than current look. |
| Tags entry / `.ao-tags-entry` | Padding and top margin. | Keep padding. Move top separation to parent `TagEditor` spacing; remove self margin-top. |

### Dialogs

| Dialog | Current State | Recommendation |
| --- | --- | --- |
| `TrackCustomViewDialog` | Semantic classes are already assigned to content, rows, section titles, lists. | Good direction. Define spacing in CSS classes and keep C++ `set_spacing()` where needed. Section title spacing should be in `.ao-custom-view-section-title`, not utility margins. |
| `SmartListDialog` | Uses local numeric margins and spacing. | Convert outer margin to `.ao-dialog-content` padding. Keep panel spacing explicit. |
| `ImportProgressDialog` | Box margin `12`; action area also margin `12`; OK button appears appended twice. | Spacing should be cleaned separately. Use `.ao-dialog-content` and `.ao-dialog-actions`; avoid stacking margins between nested boxes. |
| Export mode dialog | Uses `ao-margin-medium` utility on content box. | Replace with `.ao-export-mode-dialog-content` or shared `.ao-dialog-content`. |
| `LayoutEditorDialog` | Properties box margin `12`; section labels use `ao-margin-top-xlarge`. | Use `.ao-layout-editor-properties` for panel padding and `.ao-layout-editor-section-title` for section separation. |

### Layout Runtime Components

| Component | Current State | Recommendation |
| --- | --- | --- |
| `BoxComponent` | `spacing` prop maps to `Gtk::Box::set_spacing()`. | Keep. Long term, allow named token values (`xs`, `sm`, `md`) instead of raw integers. |
| `applyCommonProps()` margin | Supports numeric `layout.margin`. | Keep for backward compatibility, but mark as escape hatch/deprecated in editor UI. Prefer parent `spacing` and semantic classes. |
| `cssClasses` prop | Allows arbitrary classes. | Keep. Encourage semantic layout classes over utility classes in built-in layouts. |
| Error placeholder labels | Use hardcoded margin `10`. | Use a shared `.ao-layout-error` class or normalize to `12`. This is low priority. |
| Absolute canvas / tabs / split / scroll | Mostly structural. | Structural containers should not add cosmetic margins unless they represent a named region. |

## Proposed CSS Structure

Organize [app.css](/home/rocklee/dev/Aobus/app/linux-gtk/app.css) into these sections:

1. Tokens
2. Layout regions
3. Reusable primitives
4. Product components
5. Transient states
6. Legacy utilities

Example:

```css
:root {
  --ao-space-2xs: 2px;
  --ao-space-xs: 4px;
  --ao-space-sm: 6px;
  --ao-space-md: 8px;
  --ao-space-lg: 12px;
  --ao-space-xl: 16px;
  --ao-space-2xl: 24px;
}

.ao-playback-strip {
  padding: var(--ao-space-xs) var(--ao-space-sm);
}

.ao-track-controls-bar {
  padding: var(--ao-space-xs);
}

.ao-sidebar-row {
  padding: var(--ao-space-xs) var(--ao-space-sm);
}

.ao-query-completion-row {
  padding: var(--ao-space-xs) var(--ao-space-lg);
}

.ao-status-bar {
  min-height: 24px;
  padding: 1px var(--ao-space-sm);
}

.ao-dialog-content {
  padding: var(--ao-space-lg);
}
```

Keep the current utility classes temporarily, but move them under a `Legacy layout utilities` comment and avoid adding new production call sites.

## Migration Plan

### Phase 1: Fix Inconsistencies

- Remove the duplicate status bar margin from either CSS or C++; preferred target is no component-owned outer margin.
- Replace `ao-margin-top-large` with a semantic `.ao-track-section-header`.
- Fix StyleManager user CSS loading order so startup and reload precedence match.
- Change utility classes from physical `left/right` to logical `start/end` where GTK CSS supports it.

### Phase 2: Introduce Region Classes

- Add `.ao-playback-strip` to the built-in playback row.
- Add `.ao-track-controls-bar` to `TrackViewPage`.
- Add `.ao-status-region` or equivalent layout class around the status bar if edge inset is desired.
- Add `.ao-dialog-content` and `.ao-dialog-actions` for shared dialog rhythm.

### Phase 3: Replace Utility Call Sites

Replace current utility call sites:

| Current | Target |
| --- | --- |
| `ListSidebarPanel` row `ao-margin-x/y-*` | `.ao-sidebar-row` |
| `ListSidebarPanel` filter label `ao-margin-start-medium` | `.ao-sidebar-filter-label` or row spacing |
| `QueryExpressionBox` completion label utilities | `.ao-query-completion-row` |
| `PlaybackDetailsWidget` `ao-margin-x-medium` | `.ao-playback-details` or parent status spacing |
| `StatusComponents` separator `ao-margin-x-medium` | `.ao-status-separator` |
| `ImportExportCoordinator` `ao-margin-medium` | `.ao-dialog-content` |
| `LayoutEditorDialog` section title utilities | `.ao-layout-editor-section-title` |

### Phase 4: Normalize Tokens

- Replace ad hoc `10` with `8` or `12`.
- Replace chip spacing `5` with `4` or `6`.
- Keep drawing-specific values in custom widgets if they are optical rather than layout spacing.
- Update `LayoutConstants.h` to mirror the final CSS token names, or add a small named token resolver for layout documents.

## Review Checklist

Use this checklist for future UI changes:

- Does this widget set outer margin? If yes, why is the parent unable to own it?
- Is this spacing internal padding, sibling gap, or region edge inset?
- Is the value on the spacing scale?
- Is the class semantic, or is it a utility class hiding a layout decision?
- If the class uses horizontal spacing, does it preserve start/end directionality?
- Can this widget be moved to another parent without carrying unwanted whitespace?

## Decision

Do not let every component freely customize margin. Give each component a semantic class for its own visual identity, but constrain that class to internal styling by default. Use parent layouts and region classes for external rhythm. Utility margin classes remain available for layout documents and migration, but they should not be the product UI's primary aesthetic system.
