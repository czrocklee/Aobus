# Metadata and Property Editor: Design & Implementation Plan

## 1. Overview and Philosophy

The goal is to replace traditional, blocking modal dialogs for metadata editing and property viewing with a seamless, "flow-state" oriented design. 

We will adopt a **"Macro + Micro" (1+2) approach**, which is the gold standard for professional data-heavy applications (e.g., Lightroom, Apple Music, Notion):
1. **Right Inspector Sidebar (Macro):** A collapsible right panel that displays comprehensive details (Cover Art, Metadata, Audio Properties, Tags) for the currently selected track(s). Ideal for batch editing, viewing technical details, and deep tagging.
2. **ColumnView Inline Editing (Micro):** Spreadsheet-like editing directly within the `ColumnView`. Users can click a cell and edit it in place. Ideal for quick, single-track fixes without losing list context.

---

## 2. Architecture & Data Flow

To ensure a professional-grade experience, the architecture must handle asynchronous UI updates, avoid main-thread blocking during LMDB commits, provide UI state rollbacks on failure, and lay the groundwork for Undo/Redo.

### 2.1 MetadataCoordinator (The Brain)
All metadata modifications (whether from the inline list or the sidebar) route through a central `MetadataCoordinator`.
- **Command Pattern & Undo/Redo:** Modifications are encapsulated as commands (e.g., `UpdateMetadataCommand{trackIds, field, newValue}`). This abstraction allows us to build a robust Undo/Redo stack in the future.
- **Asynchronous Execution:** While LMDB features MVCC (non-blocking reads), `WriteTransaction` commits are synchronous and I/O bound. The Coordinator MUST execute these batch updates on a background thread/pool to ensure the GTK main loop (60fps rendering) never stalls, even when updating thousands of tracks.
- **Atomicity & Batching:** The Coordinator wraps batch edits in a single LMDB `WriteTransaction`. An update applying to 1,000 selected tracks succeeds entirely or fails entirely. There are no partial states.
- **Error Handling & Rollback:** If a background write fails (e.g., disk full, permission error), the Coordinator dispatches an event back to the main thread. The UI is then commanded to roll back the `Gtk::EditableLabel` to its original value, accompanied by a non-blocking UI toast/notification.

### 2.2 ColumnView Factory & State Management
`Gtk::ColumnView` heavily recycles row widgets. We cannot blindly replace `Gtk::Label` with `Gtk::EditableLabel`.
- **Lifecycle Bindings:** The `SignalListItemFactory` must explicitly manage the edit state. 
  - `bind()`: Apply the text and connect the editing signals tied to the *current* row's `TrackId`.
  - `unbind()`: Disconnect editing signals, cleanly cancel any active edits, and reset the widget state so that the recycled widget doesn't leak data into another row.
- **Focus Management:** Focus loss or scrolling out of view during an active edit must explicitly commit or cancel the edit.

---

## 3. Step-by-Step Implementation Plan

### Phase 0: Risk Mitigation & Spike (Crucial First Step)
*Objective: De-risk the most complex technical unknowns before committing to the full architecture.*
1. **Version Validation:** (Completed) `nix-shell` provides `gtkmm 4.20.0`, which safely supports `Gtk::EditableLabel` (requires GTK 4.14+).
2. **Spike 1: ColumnView Widget Recycling:** Implement a single editable column (e.g., Title) in the `TrackPresentation`. Verify that scrolling fast doesn't cause focus/data leakage or crash the application when widgets unbind while editing.
3. **Spike 2: Async Write & Revert:** Build a dummy `MetadataCoordinator` background write that intentionally fails. Verify the UI snaps back to the original value cleanly without blocking the main loop.

### Phase 1: The Inspector Sidebar Foundation
*Objective: Build the UI shell and bind it to the reactive selection model.*
1. **Create `InspectorSidebar`:** Inherits from `Gtk::Box` (vertical). Contains sections for Hero (Cover Art), Metadata (Title/Artist), Tags, and Audio Properties.
2. **Integrate into Layout:** Add a `Gtk::Revealer` (for smooth sliding) on the right side of the Content Area. Include a toggle button `(i)`.
3. **Selection Binding:** Connect to `_selectionModel->property_selection().signal_changed()`. 
   - *Empty (0):* Show empty state.
   - *Single (1):* Fetch track details and populate.
   - *Batch (>1):* Compute aggregated metadata. Matching fields show the value; differing fields show `<Multiple Values>`.

#### Hero Section (Cover Art)

The Hero section occupies the top of the Inspector and displays cover art prominently. Unlike the existing 50x50 left-sidebar thumbnail (which re-reads and re-decodes from LMDB on every selection change), the Hero introduces several new requirements:

- **Sizing:** 280x280 target, responsive to sidebar width (scales down with narrower inspector, capped at 280px). Maintains `keep_aspect_ratio` so non-square covers letterbox gracefully.
- **Pixbuf Cache:** An in-memory LRU cache (max ~50 entries, keyed by `ResourceId`) to avoid repeated LMDB reads + GdkPixbuf decodes when rapidly switching selections. Cache is cleared on library close. This replaces the current "decode on every selection change" approach which would stutter at 280x280.
- **Multi-Select Visual:** When multiple tracks are selected with differing cover art, display a "stacked cards" motif — the first track's cover at full opacity in front, with 1-2 offset "behind" layers at reduced opacity to visually communicate "multiple items." When all selected tracks share the same cover art, show it normally.
- **Empty / Missing State:** When no track is selected, or the selected track has no cover art, display a styled placeholder — a theme-aware gradient or subtle icon treatment matching the Aobus Soul design language (not the raw GTK4 empty-pixbuf placeholder).
- **Async Decode (optional/future):** For large embedded cover art (e.g., 2000x2000 PNG), decode on a background thread to avoid main-thread stalls. The current 50x50 widget decodes synchronously; this is negligible at thumbnail size but becomes noticeable at 280x280 with large source images.

### Phase 2: Core Editing & Tag Migration
*Objective: Make the Inspector functional, async-safe, and migrate the popover logic.*
1. **Implement `MetadataCoordinator`:** Build the background thread dispatcher, LMDB write transaction boundaries, and the GTK dispatcher to communicate back to the UI.
2. **Editable Labels in Sidebar:** Hook up the `Gtk::EditableLabel` widgets in the Inspector to the Coordinator.
3. **Migrate `TagPopover` UX:** 
   - *Design Shift:* A floating popover has explicit "Confirm/Cancel" semantics. An inline sidebar tag editor needs an explicit "Staged Changes" visual state (e.g., dashed outlines for pending tags) or an explicit "Apply" button to prevent accidental batch tag applications.
   - Implement the `InspectorTagSection` replacing the popover logic.

### Phase 3: Inline Editing in ColumnView
*Objective: Enable ultra-fast, spreadsheet-style edits directly in the track list.*
1. **Apply Spike Learnings:** Implement the safe `bind()`/`unbind()` logic for `Gtk::EditableLabel` across Title, Artist, and Album columns.
2. **Data Sync:** Ensure that an edit committed in the list view immediately reflects in the sidebar (and vice-versa) via the reactive model signals.

### Phase 4: "Geek" Features & Polish
*Objective: Add the advanced interactions that make Aobus unique.*
1. **Hyperlinked Properties:** Render read-only properties (Format, Sample Rate, Bit Depth) as clickable links. Clicking them automatically populates the `QueryExpressionBox` (e.g., `sample_rate == 48000`) and filters the current view.
2. **Batch Editing Logic:** Finalize the logic where entering a new value over `<Multiple Values>` in the sidebar creates a single, atomic bulk update transaction in the `MetadataCoordinator`.

---

## 4. Testing Strategy (TDD)

Following `CONTRIBUTING.md`, all modifications require rigorous test coverage.

### 4.1 Unit Tests (MetadataCoordinator)
- **Atomicity:** Mock LMDB to simulate a batch write where the 5th item fails. Verify the transaction aborts entirely and the database state is unchanged.
- **Command Abstraction:** Verify that command generation outputs the correct expected inverse commands (for future undo/redo).
- **Batch Aggregation:** Test the logic that computes `<Multiple Values>` vs a unified value correctly given varying sets of `Track` models.

### 4.2 Integration & UI State Tests
- **Factory Recycling:** Programmatically simulate the `Gtk::ListView` recycling events (`setup` -> `bind` -> `unbind` -> `teardown`). Verify that signals are disconnected and memory does not leak.
- **Rollback Behavior:** Trigger an edit in a `Gtk::EditableLabel`, force the `MetadataCoordinator` to reject the change, and assert the UI label returns to the pre-edit value.
