# Linux-GTK UI Design and Interaction Logic

> **Audience:** This document is written for UI designers and UX officers. It describes user-facing appearance, behavior, and interaction patterns — not implementation details. Class names, file paths, and data pipelines belong elsewhere.

## 1. Overview

Aubus is a GTK4 music application with a sidebar navigation, track list display, and playback controls.

### Architecture

- **Model**: Track data, playlists, smart lists
- **View**: GTK4 widgets (TreeView, ColumnView, etc.)
- **Controller**: Coordinator classes for playback, tag editing, import/export

---

## 2. Main Window Layout

```
┌──────────────────────────────────────────────────────────────────┐
│ File Menu                                                         │
├──────────────────────────────────────────────────────────────────┤
│ Playback Bar (transport controls, seek, volume)                   │
├──────────┬───────────────────────────────┬────────────────────────┤
│          │                               │                        │
│ Sidebar  │         Content Area          │  Inspector Sidebar     │
│ (Lists)  │    (Track list per selected   │  (collapsible)         │
│          │     list, inline editable)    │                        │
│          │                               │  Hero · Metadata       │
│          │                               │  Audio Props · Tags    │
│ ─ ─ ─ ─ │                               │                        │
│Cover Art │                               │                        │
└──────────┴───────────────────────────────┴────────────────────────┘
│ Status Bar (global, persistent across all views)                  │
└──────────────────────────────────────────────────────────────────┘
```

- **Sidebar**: List tree navigation, fixed width 330px (see Section 3)
- **Content Area**: Stack-based track list pages, switches on sidebar selection (see Section 4)
- **Inspector Sidebar**: Collapsible right panel for track details and metadata editing (see Section 6)
- **Cover Art**: 50x50px thumbnail at bottom of left sidebar (see Section 5)
- **Playback Bar**: Transport controls, always visible at top (see Section 7)
- **Status Bar**: Global status, always visible at bottom (see Section 8)

---

## 3. Sidebar Navigation

Left panel displays library lists in a **TreeView** structure:

- All Tracks (built-in)
- Smart Lists (user-created with filter expressions)
- Manual Playlists (user-created)

**Interactions:**
- Click list → Show its track page in content area
- Right-click list → Context menu (New / Edit / Delete)
- Double-click filter icon → Create smart list with that filter

---

## 4. Track List Display

Each list displays its tracks in a **ColumnView** (sortable, multi-select):

| Column | Description | Editable |
|--------|-------------|----------|
| Title | Track name | Long-press |
| Artist | Artist name | Long-press |
| Album | Album name | Long-press |
| Genre | Genre name | — |
| Duration | Track length | — |
| Tags | User tags | — |
| ... | Other metadata | — |

**Controls Bar (above list):**
- Filter input field (text search / expression)
- Group-by dropdown (None, Artist, Album, Genre, Year, etc.)
- Columns button (toggle column visibility)

**Interactions:**
- Click row → Select track
- Double-click row / Enter → Start playback
- Long-press Title/Artist/Album cell → Inline edit (see Inline Editing below)
- Drag Artist/Album/Genre value onto filter entry → Populate filter expression (see Drag-to-Filter below)
- Right-click row → Tag editing popover
- Double-click Tags cell / Ctrl+T → Open tag editor
- Click column header → Sort by that column
- Drag column header → Reorder columns
- Resize column edge → Resize column

**Features:**
- Filter error display (below controls bar)
- Grouped sections when Group-by is active
- Playing track highlight (beam effect)

### Inline Editing

Title, Artist, and Album cells switch to an edit field on **long-press** (~600ms):

- **Activate**: Long-press a cell → the label crossfades into a text entry with all text selected
- **Commit**: Press Enter or move focus away → value written to the model and dispatched to a background worker for database update
- **Cancel**: Press Escape → reverts to the original value, no write occurs
- **Optimistic update**: The UI updates immediately; if the database write fails, the cell reverts to its pre-edit value

### Drag-to-Filter

Artist, Album, and Genre columns support drag-and-drop onto the filter entry. Dragging a cell value onto the filter entry creates a corresponding filter expression (e.g., dragging "The Beatles" from the Artist column sets `artist:"The Beatles"` in the filter).

---

## 5. Cover Art

A 50x50 thumbnail displayed at the bottom of the left sidebar, anchored to the lower edge below the list tree. It reflects the cover art of the currently selected track.

```
┌──────────────┐
│              │
│   Sidebar    │
│   (Lists)    │
│              │
│              │
├──────────────┤
│  ┌────────┐  │
│  │        │  │
│  │ Cover  │  │
│  │  Art   │  │
│  └────────┘  │
└──────────────┘
```

**Behavior:**

| Selection State | Cover Art Display |
|-----------------|-------------------|
| No selection | Placeholder (empty frame, "No cover art" alt text) |
| Single track with cover art | Track's cover art, scaled to fit 50x50, aspect ratio preserved |
| Single track without cover art | Placeholder |
| Multiple tracks | Cover art of the **first** selected track |

**Visual:**
- Aspect ratio preserved (image is scaled to fit within the 50x50 area without cropping)
- No border, no rounded corners, no hover effects
- Aligned to the bottom of the left sidebar panel

### Data Source

Cover art is extracted from audio files during import (FLAC, MP4, MP3 formats) and stored in the library database. Each track carries a reference to its cover art; tracks from the same album with identical cover art data share a single stored copy.

Cover art is not currently shown in the Playback Bar or Status Bar during playback.

The Inspector Sidebar (see Section 6) displays a larger 280x280 hero cover art for the selected track, backed by an in-memory LRU cache.

---

## 6. Inspector Sidebar

A collapsible right panel that displays detailed information and provides editing capabilities for the currently selected track(s). It slides in from the right via a `Gtk::Revealer` with a slide animation.

The sidebar is toggled by a narrow handle button between the content area and the sidebar. It is hidden by default.

### Sections

```
┌─────────────────────┐
│                     │
│    ┌───────────┐    │  ◀ Hero
│    │ Cover Art │    │    280x280 dominant image
│    │           │    │
│    │"No Artwork"│   │    Placeholder when no art
│    └───────────┘    │
│                     │
│  TITLE              │  ◀ Metadata (editable)
│  Track Title        │    Click to edit
│  ARTIST             │    Enter / click-away to commit
│  Artist Name        │
│  ALBUM              │
│  Album Name         │
│                     │
│  AUDIO PROPERTIES   │  ◀ Audio (read-only)
│  Format      FLAC   │
│  Sample Rate 48 kHz │
│  Channels    2 Ch   │
│  Duration    3:45   │
│                     │
│  ┌─ TAGS ─────────┐ │  ◀ Tags (chip editor)
│  │ Search or add..│ │    Toggle chips to add/remove
│  │ ┌──┐ ┌──┐ ┌──┐ │ │
│  │ │Rock│ │Jazz│ │..││
│  │ └──┘ └──┘ └──┘ │ │
│  │ ────────────── │ │
│  │ Available tags  │ │
│  │ ┌──┐ ┌──┐     │ │
│  │ │Pop│ │Live│   │ │
│  │ └──┘ └──┘     │ │
│  └────────────────┘ │
│                     │
└─────────────────────┘
```

### Behavior by Selection State

| Selection | Hero | Metadata | Audio | Tags |
|-----------|------|----------|-------|------|
| None | Hidden | Hidden | Hidden | Hidden |
| Single track | Cover art (280x280) or "No Artwork" placeholder | Populated with track values, ready to edit | Format, sample rate, channels, duration | Current tags shown as chips |
| Multiple tracks | "No Artwork" placeholder | Aggregated: matching values displayed, differing fields show `<Multiple Values>` | Hidden | Common tags shown, batch edit enabled |

### Hero Cover Art

- **Size**: 280x280px target, constrained by sidebar width
- **Cache**: In-memory LRU cache (100 entries) avoids repeated disk reads when switching selections
- **Single selection with art**: Decoded from LMDB resource store, scaled to fit via `Gdk::Pixbuf`
- **Single selection without art**: "No Artwork" label displayed
- **Multi-selection**: Placeholder shown (future: stacked-cards motif)

### Metadata Editing

Title, Artist, and Album fields are rendered as editable labels. Editing flow:

1. Click the label → enters editing mode
2. Type new value, then click away or press Enter → commits
3. The UI updates optimistically; the write is dispatched to a background worker
4. If the write fails, the field reverts to its previous value

### Audio Properties

Read-only display of technical audio properties derived from the track's stored property data:

- **Format**: Codec name (FLAC, MP3, etc.)
- **Sample Rate**: Formatted as kHz (e.g., "48 kHz", "44.1 kHz")
- **Channels**: Channel count with "Ch" suffix (e.g., "2 Ch" for stereo)
- **Duration**: Formatted as `m:ss` or `h:mm:ss`

### Tags

Tags are managed through an embedded chip-based editor:

- **Current tags** (highlighted chips): Tags present on ALL selected tracks. Click to remove.
- **Available tags** (dimmed chips): Top 50 most frequent tags across the library. Click to add.
- **Search/Add entry**: Type + Enter to create and add a new tag.
- Changes are committed immediately on each chip toggle. There is no separate "Apply" step.

---

## 7. Playback Bar & Status Bar

Transport controls always visible below the menu:

```
[Output ▼] [◀◀] [▶/❚❚] [▶▶]  [────────Seek────────]  [🔊 ────Vol───]
```

| Element | Description |
|---------|-------------|
| Output button | Select audio device/output |
| Previous | Previous track |
| Play/Pause | Toggle playback |
| Next | Next track |
| Seek slider | Seek to position in current track |
| Volume slider | Adjust volume |
| Mute button | Toggle mute |

**Interactions:**
- Click Output button → Opens device selection popover
- Click Play/Pause → Start or pause playback
- Click Previous/Next → Skip tracks
- Drag seek slider → Seek to position
- Drag volume slider → Adjust volume
- Click output button → Select audio backend
- Long-press output button (1s) → Easter egg (fullscreen Aobus Soul animation)

---

## 8. Status Bar

Global status bar persistent across all views:

```
┌─────────────────────────────────────────────────────────────────────┐
│ Stream Info    │          Now Playing           │ Selection │  Library │
│ [Quality Dot]  │      Artist - Title          │  X items  │ 1254 tracks │
└─────────────────────────────────────────────────────────────────────┘
```

**Left Section:**
- Stream Info label: Format info (e.g., "48.0 kHz · 32-bit · Stereo")
- Quality dot: Colored indicator reflecting audio quality
  - Purple: Bit-perfect / Lossless padded
  - Green: Lossless float
  - Amber: Linear intervention
  - Gray: Lossy source
  - Red: Clipped

**Center:**
- Now Playing label: "Artist - Title" (clickable)
  - Click → Jump to the list containing the currently playing track
  - Hover: Highlighted with theme color
  - Tooltip: "Click to show playing list"

**Right Section:**
- Selection count: "X items selected" (with duration if applicable)
- Library count: "X tracks"
- Import progress: Progress bar shown during import (hidden by default)

**Stream Info Tooltip (Quality Chain Analysis):**
```
Audio Pipeline:
• [Source] decoder-name (48.0 kHz · 32-bit · Stereo)
• [Engine] engine-name
• [Stream] stream-name
• [Device] device-name

Quality issues (if any):
• Mixed: Shared with other apps
• Resampling: 44100Hz → 48000Hz
• Volume: Modified
• etc.

Conclusion: Bit-perfect output
```

**Playback State Behavior:**
- Idle: Shows "Connecting to audio engine..." if not ready
- Playing: Shows artist/title, format info, quality indicator
- Underruns: Appends "N underruns" if buffer underruns occur

---

## 9. Dialogs and Popovers

### TagPopover

**Trigger:** Right-click on track row, or Ctrl+T / double-click Tags cell

**Appearance:** Small popover anchored to the triggering widget

**Content:**
- Search/add tags field
- Current tags (highlighted chips — tags on all selected tracks)
- Available tags (dimmed chips — top suggestions from library)

**Interaction:** Click tag chip to toggle (add/remove), Enter in search field to create and add a new tag. Changes are committed immediately after each toggle; there is no separate "Apply" step.

The same tag editing UI is also used inline in the Inspector Sidebar (see Section 6).

---

### SmartListDialog

**Trigger:** New Smart List from sidebar context menu, or double-click filter icon

**Appearance:** Modal dialog

**Content:**
- Name and description fields
- Filter expression input (with autocomplete)
- Live preview of matching tracks
- Match count

**Interaction:** Type expression → See preview update → Save

---

### ImportProgressDialog

**Trigger:** File → Import Files

**Appearance:** Modal dialog with progress bar

**Content:** Progress bar, current file name, OK button when complete

---

### ExportModeDialog

**Trigger:** File menu (future)

**Appearance:** Modal dialog

**Content:** Dropdown with three options (App-Only, Metadata+App, Full Backup)

---

### ColumnsPopover

**Trigger:** Click Columns button in track list controls bar

**Appearance:** Small popover

**Content:** Checkboxes for each column visibility, Reset button

---

### OutputPopover

**Trigger:** Click Output button in playback bar

**Appearance:** Popover (360x320) with scrolled list of audio devices

**Content:**
- Section headers for each backend (e.g., "PipeWire", "ALSA")
- Device rows with name and optional description
- Checkmark indicator for active device
- "[E]" suffix on devices supporting exclusive mode

**Interaction:** Click device → Switch audio output

**Tooltip (idle):** "Click for devices, hold right-click for Soul"

---

### Quality Indicator

Color-coded badge showing audio quality throughout the playback chain:

| Quality | Color | Description |
|---------|-------|-------------|
| BitwisePerfect | Purple (#A855F7) | No processing, bit-perfect output |
| LosslessPadded | Purple (#A855F7) | Lossless with padding |
| LosslessFloat | Green (#10B981) | Float format, lossless |
| LinearIntervention | Orange (#F59E0B) | Some processing applied |
| LossySource | Gray (#6B7280) | Compressed source (MP3, AAC, etc.) |
| Clipped | Red (#EF4444) | Audio clipping detected |

**Display Location:** Status Bar (and Aobus Soul window during easter egg)

---

### Quality Chain Analysis Tooltip

**Trigger:** Hover on quality indicator in Status Bar

**Content:** Complete audio pipeline analysis showing each node in the playback chain:

```
Audio Routing Analysis:
• [Source] ao-decoder (format info)
• [Engine] ao-engine (format info)
• [Stream] pipewire-xxx (format info) [Vol Control] [Muted]
• [Device] alsa_output.xxx (format info)

• Mixed: PipeWire Stream shared with {app1, app2}  <-- if mixed with other apps
• Source: Lossy format (name)                        <-- if lossy source
• Volume: Modification at node                       <-- if volume changed
• Resampling: 44100Hz → 48000Hz                    <-- if sample rate conversion
• Channels: 2ch → 2ch                              <-- if channel conversion
• Bit-Transparent: Float mapping                   <-- if lossless float conversion
• Precision: Truncated 24b → 16b                   <-- if lossy conversion

Conclusion: Bit-perfect output                      <-- final verdict
```

**Node Types:**
- [Source] - Decoder node
- [Engine] - Engine node
- [Stream] - Stream node (PipeWire/PulseAudio)
- [Filter] - Intermediary/filter node
- [Device] - Sink node (hardware)
- [Other Source] - External source node
- [Unknown] - Unknown type

---

### Aobus Soul Easter Egg

**Trigger:** Long-press right-click (1 second) on Output button

**Effect:** Fullscreen modal overlay displaying animated "Aobus Soul" logo

**Animation Features:**
- GPU-accelerated GSK rendering
- "Aura Flow" color gradient with subtle hue shifting during playback
- Golden Ratio-based animation periods:
  - Breathing: ~5.1s
  - Rotation: ~8.3s
  - Opacity: ~13.4s
  - Hue: ~21.7s
- Opacity pulses between 0.8 and 1.0
- Stroke width pulses with breathing animation

**Visual Design:**
- Semi-transparent black background (rgba 0,0,0,0.85)
- Aspect ratio 147:65
- Size based on monitor geometry using Golden Ratio
- Tick-based animation at 400px default height

---

## 10. Context Menus

### Track Context Menu

**Trigger:** Right-click on track row

**Options:**
- Edit Tags → Opens TagPopover

---

### List Context Menu

**Trigger:** Right-click on list in sidebar

**Options:**
| Option | Result |
|--------|---------|
| New Smart List... | Opens SmartListDialog |
| Edit List... | Opens edit mode of SmartListDialog |
| Delete List | Removes list (confirmation for non-empty) |

---

## 11. Keyboard Shortcuts

| Key | Action |
|-----|--------|
| Enter | Start playback of selected track |
| Ctrl+T | Open tag editor for selected tracks |

---

## 12. File Menu

| Item | Action |
|------|--------|
| Open Library | Open existing library file |
| Import Files | Import audio files into library |
| Quit | Exit application |

---

## 13. State Persistence

Application saves and restores on restart:
- Window size and position
- Sidebar width (paned divider)
- Column visibility and widths
- Selected audio backend

---

## 14. Visual Design

### Styling
- Inline CSS for custom styling (playing beam effect)
- Dynamic CSS variables for animated effects
- Theme-aware coloring

### Layout Constants
- Spacing: 4px / 6px / 8px / 12px
- Icon sizes: 16px / 12px
