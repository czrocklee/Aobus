# Track Properties Dialog Design

## Overview

Add a `TrackPropertiesDialog` (extends `Gtk::Dialog`) with a `Gtk::Notebook` providing two tabs:

- **Metadata tab** — editable fields for all tag metadata
- **Properties tab** — read-only display of audio/codec/file properties

Transform the right-click on the track view from directly opening a `TagPopover` into a `Gtk::PopoverMenu` with "Edit Tags" and "Properties" menu items.

The dialog must keep metadata and properties clearly separated: the Metadata tab contains only editable tag fields, while the Properties tab contains only read-only file/audio information.

---

## Files to Modify

| Action | File | Reason |
|--------|------|--------|
| **NEW** | `app/linux-gtk/tag/TrackPropertiesDialog.h` | Dialog header |
| **NEW** | `app/linux-gtk/tag/TrackPropertiesDialog.cpp` | Dialog implementation |
| **MODIFY** | `app/runtime/StateTypes.h:227-235` | Extend `MetadataPatch` with more fields (albumArtist, year, trackNumber, discNumber, etc.) |
| **MODIFY** | `app/runtime/LibraryMutationService.cpp:44-86` | Extend `applyMetadataPatch` to handle new fields |
| **MODIFY** | `app/linux-gtk/tag/TagEditController.h` | Add method to create/show `TrackPropertiesDialog` |
| **MODIFY** | `app/linux-gtk/tag/TagEditController.cpp` | Implement `Gtk::PopoverMenu` context menu with "Edit Tags" / "Properties" items |
| **MODIFY** | `app/linux-gtk/CMakeLists.txt` | Add `tag/TrackPropertiesDialog.cpp` |

---

## `TrackPropertiesDialog` Layout

### Metadata Tab

```
┌──────────────────────────────────────────────────────────┐
│  Properties — 3 tracks selected                          │
├──────────────┬───────────────────────────────────────────┤
│ [Metadata]   │  Properties                               │
├──────────────┴───────────────────────────────────────────┤
│                                                          │
│  Track Title:  [Bohemian Rhapsody                    ]   │
│  Artist:       [Queen                                ]   │
│  Album Artist: [Queen                                ]   │
│  Album:        [A Night at the Opera                 ]   │
│  Genre:        [Rock                                 ]   │
│  Composer:     [Freddie Mercury                      ]   │
│  Work:         [                                     ]   │
│  Year:         [1975                                 ]   │
│  Track Number: [11]  /  [12]                             │
│  Disc Number:  [1]   /  [1]                              │
│                                                          │
├──────────────────────────────────────────────────────────┤
│                                    [Save]    [Close]      │
└──────────────────────────────────────────────────────────┘
```

### Properties Tab

```
┌──────────────────────────────────────────────────────────┐
│  Properties — 3 tracks selected                          │
├──────────────┬───────────────────────────────────────────┤
│  Metadata    │ [Properties]                              │
├──────────────┴───────────────────────────────────────────┤
│                                                          │
│  File Path:  /home/user/music/queen/br.mp3               │
│  Codec:      MP3                                         │
│  Sample Rate: 44.1 kHz                                   │
│  Channels:   2 Ch                                        │
│  Bit Depth:  16 bit                                      │
│  Bitrate:    320 kbps                                    │
│  Duration:   5:54                                        │
│  File Size:  13.2 MB                                     │
│  Modified:   2024-01-15 14:30                            │
│                                                          │
├──────────────────────────────────────────────────────────┤
│                                    [Save]    [Close]      │
└──────────────────────────────────────────────────────────┘
```

---

## Multi-Track Editing Behavior

- If all selected tracks have the same value for a field, show that value.
- If values differ, the Entry shows greyed-out `<Multiple Values>`; user editing applies to all.
- In read-only properties, differing values display as `Mixed`.

---

## Metadata Tab — All Editable (after extending MetadataPatch)

| Field | Widget | Source (TrackView) |
|-------|--------|---------------------|
| Track Title | `Gtk::Entry` | `metadata().title()` |
| Artist | `Gtk::Entry` | `metadata().artistId()` -> dict |
| Album Artist | `Gtk::Entry` | `metadata().albumArtistId()` -> dict |
| Album | `Gtk::Entry` | `metadata().albumId()` -> dict |
| Genre | `Gtk::Entry` | `metadata().genreId()` -> dict |
| Composer | `Gtk::Entry` | `metadata().composerId()` -> dict |
| Work | `Gtk::Entry` | `metadata().workId()` -> dict |
| Year | `Gtk::SpinButton` | `metadata().year()` |
| Track # | `Gtk::SpinButton` x2 | `metadata().trackNumber()` / `totalTracks()` |
| Disc # | `Gtk::SpinButton` x2 | `metadata().discNumber()` / `totalDiscs()` |

---

## Properties Tab — All Read-Only

| Field | Source (TrackView) |
|-------|---------------------|
| File Path | `property().uri()` |
| Codec | `property().codecId()` -> formatted |
| Sample Rate | `property().sampleRate()` -> formatted |
| Channels | `property().channels()` |
| Bit Depth | `property().bitDepth()` |
| Bitrate | `property().bitrate()` -> kbps |
| Duration | `property().durationMs()` -> formatted |
| File Size | `property().fileSize()` -> formatted |
| Modified | `property().mtime()` -> formatted |

---

## Context Menu Flow

**Before:**
```
Right-click -> TagPopover (tag editing popover)
```

**After:**
```
Right-click -> Gtk::PopoverMenu
                ├── "Edit Tags"  -> TagPopover (existing behavior)
                └── "Properties" -> TrackPropertiesDialog (new)
```

Implemented using `Gio::Menu` and `Gio::SimpleAction` (following the `Gtk::PopoverMenu` pattern in `ListSidebarPanel`).

---

## Integration with Existing Architecture

- `TrackPropertiesDialog` needs references to `library::MusicLibrary` (read) and `rt::LibraryMutationService` (write) — both already injected into `TagEditController` via `rt::AppRuntime`.
- Dialog follows Aobus dialog patterns: extend `Gtk::Dialog`, take `Gtk::Window& parent` in constructor, use `Gtk::make_managed<>` for child widgets, add buttons via `add_action_widget()`.
- Load selected tracks directly from `MusicLibrary.tracks()` with full hot+cold data for dialog contents. Do not rely on `TrackRowObject` for properties because it does not expose every required value (for example bitrate, file size, mtime, and URI).
- `TrackRowCache` may still be used for resolving `DictionaryId` to display names and should be invalidated after successful metadata edits.
- CSS classes follow existing conventions (`ao-property-label`, `ao-dialog-content`, etc.).

---

## Extended MetadataPatch

```cpp
struct MetadataPatch final {
    std::optional<std::string> optTitle{};
    std::optional<std::string> optArtist{};
    std::optional<std::string> optAlbum{};
    std::optional<std::string> optAlbumArtist{};
    std::optional<std::string> optGenre{};
    std::optional<std::string> optComposer{};
    std::optional<std::string> optWork{};
    std::optional<std::uint16_t> optYear{};
    std::optional<std::uint16_t> optTrackNumber{};
    std::optional<std::uint16_t> optTotalTracks{};
    std::optional<std::uint16_t> optDiscNumber{};
    std::optional<std::uint16_t> optTotalDiscs{};
};
```

`applyMetadataPatch` in `LibraryMutationService.cpp` is extended to handle the new `optAlbumArtist`, `optYear`, `optTrackNumber`, `optTotalTracks`, `optDiscNumber`, `optTotalDiscs` fields using the corresponding `TrackBuilder::MetadataBuilder` setters.

Hot/cold update flags must match `TrackView` storage:

- Hot metadata: title, artist, album, album artist, genre, composer, year.
- Cold metadata: work, track number, total tracks, disc number, total discs.

When applying the patch, set `changedHot` only for hot-field changes and `changedCold` only for cold-field changes. In particular, `optWork` must be treated as a cold metadata update.

---

## Testing

- Catch2 tests in `test/` verifying the `MetadataPatch` extension and `applyMetadataPatch` logic, including:
  - Hot-only edits such as album artist/year update hot data.
  - Cold-only edits such as work/track number update cold data.
  - Combined edits update both hot and cold data.
  - Unchanged or empty patches do not rewrite data unnecessarily.
- Manual test: single track -> Properties -> edit -> Save. Multi-track -> verify `<Multiple Values>` handling and batch-apply.
