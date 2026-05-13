# Track Presentation Testing Plan

## Purpose

This document defines test coverage for the presentation preset redesign.

## Runtime Unit Tests

### Built-in preset registry

File:

```text
test/unit/runtime/TrackPresentationPresetTest.cpp
```

Test cases:

1. Registry contains expected ids:
   - `songs`
   - `albums`
   - `artists`
   - `album-artists`
   - `classical-composers`
   - `classical-works`
   - `genres`
   - `years`
   - `tagging`
2. `defaultTrackPresentationSpec()` returns `songs`.
3. Each built-in has exact group-by.
4. Each built-in has exact sort terms and directions.
5. Each built-in has exact visible fields.
6. Each built-in has exact redundant fields.
7. Lookup by unknown id returns null or falls back only in the explicit fallback API.
8. Normalization removes duplicate fields.

### ViewService presentation state

File:

```text
test/unit/runtime/ViewServiceTest.cpp
```

Test cases:

1. New track-list view uses default presentation.
2. `setPresentation(viewId, "albums")` updates state.
3. `setPresentation()` increments revision.
4. `setPresentation()` publishes `PresentationChanged`.
5. Unknown id falls back to `songs` if using id-based setter.
6. Projection after `setPresentation("albums")` sorts by AlbumArtist, Album, Disc, Track, Title.
7. Projection after `setPresentation("classical-composers")` groups by Composer.
8. Compatibility `setGrouping()` remains correct until removed.

### Projection snapshot

File:

```text
test/unit/runtime/TrackListProjectionTest.cpp
```

Test cases:

1. `presentation()` includes presentation id.
2. `presentation()` includes visible fields.
3. `presentation()` includes redundant fields.
4. Projection grouping still creates section labels.
5. Projection sorting remains stable.
6. Projection deltas remain reset/range-based and contain no row values.

## GTK Unit Tests

### Field-to-column mapping

File:

```text
test/unit/linux-gtk/track/TrackPresentationTest.cpp
```

Test cases:

1. `TrackPresentationField::Title` maps to `TrackColumn::Title`.
2. `Artist` maps to `Artist`.
3. `Album` maps to `Album`.
4. `AlbumArtist` maps to `AlbumArtist`.
5. `Genre` maps to `Genre`.
6. `Composer` maps to `Composer`.
7. `Work` maps to `Work`.
8. `Year` maps to `Year`.
9. `DiscNumber` maps to `DiscNumber`.
10. `TrackNumber` maps to `TrackNumber`.
11. `Duration` maps to `Duration`.
12. `Tags` maps to `Tags`.

### Layout generation

Test cases:

1. `songs` layout order is Title, Artist, Album, Duration, Tags.
2. `albums` layout order is TrackNumber, Title, Duration, Year, Tags.
3. `artists` layout order is Album, TrackNumber, Title, Duration, Tags.
4. `classical-composers` layout order is Work, Title, Artist, Album, Duration, Year.
5. `tagging` layout order is Title, Artist, Album, Genre, Year, Tags.
6. Redundant fields are hidden or omitted according to chosen implementation.
7. Generated layouts normalize missing non-visible columns safely.

### Custom view store

If `TrackPresentationStore` is added:

```text
test/unit/linux-gtk/track/TrackPresentationStoreTest.cpp
```

Test cases:

1. Built-in lookup works.
2. Custom lookup works.
3. Custom overrides do not mutate built-ins.
4. Invalid custom definitions normalize safely.
5. Removing a custom definition emits changed signal.

### Custom view serialization round-trip

Test cases:

1. Serialize and deserialize a custom definition and compare equality.
2. Deserialize a definition with unknown field ids and confirm they are dropped.
3. Deserialize a definition with empty visible fields and confirm fallback to base preset.
4. Deserialize a definition with empty sort terms and confirm fallback to base preset.
5. Deserialize a definition with duplicate visible fields and confirm deduplication.

## Manual Integration Checks

Run after GTK toolbar phase:

1. Launch app.
2. Open All Tracks.
3. Confirm toolbar shows filter + View selector only.
4. Select Songs.
5. Select Albums.
   - Album sections appear.
   - Album-oriented columns appear.
6. Select Artists.
   - Artist sections appear.
7. Select Classical: Composers.
   - Composer sections appear.
   - Work column appears.
8. Select Tagging.
   - Genre/Year/Tags are visible.
9. Select Years.
   - Year sections appear.
10. Type in filter entry.
11. Create smart list from filter icon.
12. Select rows.
13. Press Enter/double-click to play.
14. Inline edit Title/Artist/Album.
15. Open tag editor from context menu.
16. Switch lists and verify view state behavior.
17. Switch preset while a track is playing.
    - Playing track continues without interruption.
    - Playback state (transport, position) is unaffected.
18. Restart app and verify persisted presentation behavior once implemented.

## Regression Risks

### Runtime risks

- Projection no longer receives correct sort terms.
- Group sections become inconsistent with sort order.
- Old `setGrouping()` tests fail during migration.
- View state has duplicate group/sort/presentation sources of truth.

Mitigation:

- Keep a single conversion helper between spec and state.
- Update tests before deleting compatibility paths.

### GTK risks

- Column layout application fights saved global column state.
- Redundant field hiding removes expected columns.
- Presentation menu button triggers selection during popover construction.
- Column widths reset too aggressively.

Mitigation:

- Block selection handler while populating menu items.
- Apply presentation layout after columns are created.
- Keep width persistence out of first version.

### Data loading risks

- Accidentally moving row value loading into runtime.
- Duplicating row caches between runtime and GTK.
- Resolving dictionary strings in projection for display rather than sort keys.

Mitigation:

- Add review checklist item: no runtime display-value API.
- Keep `TrackRowCache` unchanged through phase 8.
- Projection tests assert deltas contain no row values.

## Verification Commands

Targeted runtime:

```bash
nix-shell --run "cmake --build /tmp/build/debug --target ao_test --parallel"
nix-shell --run "/tmp/build/debug/test/ao_test [runtime]"
```

Targeted GTK track tests:

```bash
nix-shell --run "cmake --build /tmp/build/debug --target ao_test --parallel"
nix-shell --run "/tmp/build/debug/test/ao_test [linux-gtk][track]"
```

GTK library build:

```bash
nix-shell --run "cmake --build /tmp/build/debug --target aobus-gtk-lib --parallel"
```

Full milestone check:

```bash
./build.sh debug
```
