# Linux GTK Classic + Modern Preset Aesthetic Brainstorm

## Purpose

Capture visual and UX ideas for offering two built-in Linux GTK layout presets
at the same time:

- **Classic**: keep the current foobar2000-style, information-dense,
  power-user layout.
- **Modern**: add a contemporary music-player layout with stronger visual
  hierarchy, softer surfaces, and a bottom-bar now-playing control center.

This is a brainstorm, not an implementation plan. Ideas here should be promoted
into `doc/plan/` only after scope, dependencies, and tests are clarified.

## Promoted To Plans

The following ideas have enough scope clarity to move from brainstorm to
implementation planning:

- **Modern preset gap roadmap**:
  `doc/plan/linux-gtk-modern-preset/01-gap-roadmap.md`
- **Modern bottom bar structure**:
  `doc/plan/linux-gtk-modern-preset/02-bottom-bar-gap-plan.md`
- **Modern shell/table baseline**:
  `doc/plan/linux-gtk-modern-preset/03-shell-table-gap-plan.md`
- **Preset isolation and validation**:
  `doc/plan/linux-gtk-modern-preset/04-preset-isolation-validation-plan.md`
- **Modern table row polish**:
  `doc/plan/linux-gtk-modern-preset/05-table-row-polish-plan.md`
- **Modern content card surface**:
  `doc/plan/linux-gtk-modern-preset/06-content-card-surface-plan.md`

## Preset Philosophy

### Classic preset

Classic should remain the stable, compact, power-user default unless the project
decides otherwise. Its strengths are density, directness, and low visual chrome.

Classic should emphasize:

- compact rows and controls
- top playback strip
- bottom status strip
- visible library/navigation surfaces
- familiar desktop application behavior
- minimal surprise for existing users

Classic should not be forced to inherit every Modern visual treatment. Shared
component improvements are fine, but avoid turning Classic into a partially
modernized hybrid.

### Modern preset

Modern should feel like a contemporary music player while preserving Aobus'
library-management power. Its strengths should be visual hierarchy, relaxed
spacing, stronger now-playing identity, and polished interaction states.

Modern should emphasize:

- integrated bottom now-playing/control bar
- stronger artwork/image presence
- softer cards and panels
- rounded/pill controls
- subtle accent usage
- clean side navigation
- a polished selected-track inspector/details drawer

Modern should remain functional at library scale. Do not sacrifice table
legibility or sorting/filtering affordances for pure decoration.

## High-Impact Modern Enhancements

### 1. Main workspace as a subtle content card

Wrap the track table/workspace visually in a modern content surface:

- rounded corners
- subtle border
- slightly elevated/background-tinted surface
- internal padding around the table area
- reduced hard separators

This would make the central library feel intentionally composed rather than like
a raw table attached directly to the window background.

Potential CSS names:

- `ao-modern-content-shell`
- `ao-modern-content-card`
- `ao-modern-workspace`

Classic should keep its current flatter/dense structure.

### 2. Sidebar as navigation rail

Modern's left sidebar can become a calmer navigation rail:

- more vertical row padding
- rounded selected-list pill
- muted secondary counts
- section headers for list groups, if/when supported
- subtle hover background
- minimal borders

The sidebar should read as navigation, not as the primary metadata/artwork area.
Selected-track image and metadata can live in the inspector/details surface.

Potential CSS names:

- `ao-modern-sidebar`
- `ao-modern-sidebar-row`
- `ao-modern-sidebar-selected`

### 3. Track table row polish

The table is still the core of Aobus, so modernizing it has a large visual
payoff:

- slightly taller rows in Modern
- soft row hover highlight
- selected row with gentle accent background
- currently playing row with a thin accent marker
- current title slightly bolder
- optional play/equalizer glyph in a leading column later

Keep this CSS-driven first. More advanced playing indicators can be a separate
feature once row rendering contracts are clear.

Potential CSS names:

- `ao-modern-track-table`
- `ao-modern-track-row-playing`
- `ao-modern-track-row-hover`

### 4. Stronger now-playing identity

The bottom bar thumbnail is the anchor, but the app should also connect the
playing track back to the library:

- accent marker on currently playing table row
- subtle playing state in status/details surfaces
- title/artist hierarchy in the bottom bar
- clear stopped/idle placeholder state

This makes the user feel oriented: the bottom bar says what is playing, and the
library shows where it lives.

### 5. Inspector as a details drawer

For Modern, the inspector can become the rich selected-track surface:

- larger artwork/image at the top
- card sections for metadata, tags, and audio details
- compact label/value rows
- subtle section dividers
- empty state when no track is selected

This allows the left sidebar to stay focused on navigation while the right side
handles browsing/tagging details.

Potential CSS names:

- `ao-modern-inspector`
- `ao-modern-inspector-hero`
- `ao-modern-inspector-section`
- `ao-modern-inspector-metadata-row`

## Smaller Polish Ideas

### Typography pass

Modern should use clearer type hierarchy:

- slightly larger now-playing title
- muted artist/album metadata
- fewer bold labels
- consistent small text for status/counts
- opacity and spacing instead of excessive separators

Classic can stay closer to GTK defaults for compactness.

### Radius system

Use the existing CSS variables consistently:

- small radius for thumbnails/images
- medium radius for cards and table containers
- full radius for play buttons, pills, and search/filter fields

Avoid mixing arbitrary radii unless a component has a specific reason.

### Quick filter as search pill

Modern can make the quick filter feel like a command/search field:

- rounded/pill shape
- internal horizontal padding
- subtle focus ring
- optional search icon if supported cleanly
- presentation button styled as an adjacent compact action

Potential CSS names:

- `ao-modern-search-pill`
- `ao-modern-track-controls-bar`

### Softer empty/loading states

Modern should make empty and transient states feel designed:

- no library content
- no selection
- no playing track
- scanning/loading
- failed image load

Good empty states usually have a small icon, short title, muted explanation, and
one obvious action. Keep Classic simple and unobtrusive.

### Reduced chrome

Modern should rely more on spacing, surface color, and subtle shadows/borders
than on visible separators. Use hard separator lines sparingly.

## More Ambitious Modern Concepts

These are probably separate plans/features, not part of the first Modern preset
implementation.

### Album/list header mode

When viewing an album, artist, playlist, or list, show a header above the table:

- large image/artwork
- title
- subtitle/count/duration
- prominent play button
- optional context actions

This would make Aobus feel closer to modern music apps while staying library
first.

### Queue / Up Next drawer

Add a collapsible right drawer for queue/up-next. This pairs naturally with the
bottom now-playing bar but likely needs playback sequence UX work first.

### Adaptive bottom bar

At narrower widths, Modern could hide or collapse secondary controls:

- keep image, title, and play visible
- collapse output/device details
- reduce status text
- maybe hide volume behind a popover

This may need more layout/runtime support than plain YAML and CSS.

### Theme variants

Possible future built-in theme/preset combinations:

- Classic
- Modern Light
- Modern Dark
- Modern OLED / high-contrast dark

Keep layout preset selection distinct from theme selection unless the UX calls
for a combined picker.

## Suggested Prioritization

After implementing the Modern bottom bar preset, the best next aesthetic layers
are:

1. Modern track table row and currently-playing styling
2. Main workspace content card/surface
3. Quick filter/search pill polish
4. Sidebar navigation rail styling
5. Inspector/details drawer polish

This order should create the largest “modern music player” impression without
requiring a new layout engine or broad feature work.

## Guardrails

- Ship Classic and Modern side-by-side; do not replace Classic.
- Keep Classic compact and familiar.
- Prefer preset-specific CSS classes for Modern-only styling.
- Reuse existing components before adding new widgets.
- Keep component names aligned with the current image refactor
  (`ImageWidget`, `ImageCache`, `playback.image`).
- Promote individual ideas into implementation plans only when their scope,
  tests, and compatibility behavior are clear.
