---
id: tui.interaction
type: spec
status: current
domain: presentation
summary: Defines TUI text-input modes, panels, selection, mouse dispatch, playback dock, seek rail, completion, notification, and rendering behavior.
---
# TUI interaction specification

## Scope

This specification owns the terminal frontend's shell and interaction behavior.
It defines workspace structure, modal text-input and overlay state, keyboard and mouse routing, panel mechanics, playback dock and seek rail, completion, notifications, selection, and terminal styling.
Exact startup options, keys, commands, and aliases belong to the [TUI command reference](../../reference/tui/command.md).

## Code boundary

TUI code under `app/tui/` owns FTXUI elements, terminal geometry, hit regions, input dispatch, frame timing, terminal cover rendering, and TUI-local shell state.
It consumes `AppRuntime` and shared UIModel policies for presentation, seek gestures, status, output, quality, column widths, soul animation, and neutral keymaps.

`LibraryController` adapts runtime workspace/views into terminal rows but does not become library storage or playback authority.
Its list chooser consumes the shared [list-navigation tree](../presentation/list-tree.md) instead of deriving parent relationships or sibling order.
`TuiKeymapPlan` is the immutable frontend projection from an effective neutral keymap to executable FTXUI events and display chords.
`EventController` translates terminal events from that plan or from fixed scoped protocol into runtime/UIModel commands.
`CoverArtLoader` owns one cancellable selection-settle window and one cancellable selected-resource request; byte reads and cover transforms run off the screen executor and publish only for the current resource generation.

## Terminology

- **Quick Filter input** is the live filter editor entered by its effective shortcut (shipped as `/`).
- **Command Palette input** is the command editor entered by its effective shortcut (shipped as `:`).
- **Text input** means either of those mutually exclusive shell modes.
- An **overlay** is one of list, detail, quality, output, presentation, notification, or help panels.
- A **modal** overlay blocks workspace input beneath it; a **visible** overlay merely occupies the screen. The detail inspector is the only visible overlay that is not modal.
- A **hit region** is a rendered FTXUI box retained for the next mouse-dispatch pass.
- The **seek rail** is only the reflected timeline/thumb segment, excluding elapsed/duration text.
- A **visual row** includes group headers as well as selectable track rows.

## Invariants

- Text input is modal, and every overlay except the detail inspector is modal: workspace-only input cannot mutate the track table beneath them.
- Only one overlay is active at a time; opening another overlay replaces the detail inspector, while text input leaves it open beneath the suspended workspace.
- A surface change retires pointer gestures: opening or closing any overlay, and entering text input, cancels an active seek, scrollbar, or column drag rather than letting it finish against a layout the user did not aim at.
- Detail pane width follows locale and terminal width only. Selection, cover presence, and optional-field presence cannot change it.
- Render code writes hit regions into the one `TuiHitRegions` owner; input reads that same frame state.
- Shared list navigation handles arrows, pages, home, and end before a panel-specific selection callback.
- Fixed input, list, modal-overlay, notification, mouse, and escape protocol is resolved before configurable root actions; Ctrl-C remains an unconditional emergency exit.
- One prepared `TuiKeymapPlan` drives root dispatch and every hint for a configurable action, so rebinding or unbinding cannot leave a hard-coded execution or display path behind.
- Selection always resolves to a track even when scrollbar geometry counts group headers.
- Equivalent playback, presentation, filtering, notification, and output actions use shared runtime/UIModel authorities.
- The list chooser preserves the shared list-tree parent recovery and sibling order; TUI code owns only terminal flattening and decoration.
- Startup attaches the exact active runtime view restored by `WorkspaceService`; a valid empty projection does not cause replacement navigation.
- Reload materializes the same active view id and preserves its filter, presentation, grouping, sorting, history identity, filter draft, selected track when that track remains visible, and independent list/presentation chooser highlights.
- Soul/Space transport toggles and ordinary stop requests use `PlaybackActions`; explicit selected-track activation remains a distinct view-based sequence command.
- A modal surface arriving mid-gesture ends it: a pointer drag cannot be continued while text input or a modal overlay owns the workspace.
- A zero-duration timeline rejects pointer and relative-keyboard seek.
- Column drag previews a per-list terminal-cell layout; only normal release commits it, while any interruption rolls the preview back.
- Ordinary terminal styling inherits the terminal background; semantic roles add accents without painting broad application backgrounds.

## State model

`ShellInteractionModel` retains an explicit `None`, Quick Filter, or Command Palette input mode, UTF-8 draft text, whether that draft has been edited, completion result/selection, and active overlay.
`EventController` retains pointer drags for seek, scrollbar, and column resize plus hover state.
It also retains one cancellable generation-checked Quick Filter debounce task; all shell and library access occurs after resumption on the callback executor.
`LibraryController` retains active runtime view, terminal row snapshot, selected track index, sections, applied filter draft and error, and presentation adaptation.
The composition root retains one shared presentation catalog, one per-list presentation-preference model, one per-list column-layout model, one frontend-local store writer for the selected library, and one immutable TUI keymap plan loaded from the global application store.

Each input mode's completion result carries a replacement range, ranked items, display text, insertion text, and detail.
Quick Filter drafts delegate directly to the shared UIModel track-filter completer, which selects live values or structured expression candidates according to the same boundary as GTK.
Command Palette drafts complete commands and aliases; only the explicit `filter` command delegates its argument to the filter completer.
Presentation contexts add built-in and custom preset ids.

## Commands and transitions

### Quick Filter input

The effective `tui.library.openQuickFilter` shortcut (shipped as `/`) opens an empty Quick Filter draft without clearing or copying the currently applied filter.
Until the user edits that draft, Escape closes the input without changing the applied filter, while Return confirms the empty draft and clears the filter.
After an edit, the draft is applied after 200 milliseconds without another edit.
Every further edit or accepted completion cancels and replaces that pending generation.

Text appends as UTF-8, backspace removes one complete extended grapheme cluster, arrows cycle completion selection, and Page Up and Page Down move it by one bounded page.
Tab applies the selected completion, keeps the input open, refreshes completion, and schedules the resulting draft for live filtering.
Return first applies the selected completion when one exists, synchronously applies the resulting draft, and closes the input; confirming an untouched empty draft clears the filter.
Escape ignores the selected completion, synchronously applies the literal edited draft, and closes the input; an untouched draft preserves the existing filter.
Applying a live value replaces the active filter term with one safely quoted term.

### Command Palette input

The effective `tui.shell.openCommandPalette` shortcut (shipped as `:`) opens an empty Command Palette draft.
Text editing and completion navigation use the same grapheme, arrow, page, and Tab rules as Quick Filter input, but no debounce or live filtering occurs.
Return executes a known command or explicit command prefix without applying the highlighted completion.
An empty draft closes the palette; an unknown nonempty command remains open and reports a warning instead of changing the filter.
Escape cancels the command draft and closes the palette.

The modes intentionally assign different submission semantics to Return and Escape.
Quick Filter is a live value editor, so Return accepts its highlighted value while Escape preserves the literal text already typed; Command Palette input is not live, so Return executes only the typed command and Escape cancels it without allowing a highlight to replace that command implicitly.

The palette completes command names and aliases, presentation ids, and filter candidates only within an explicit `filter` argument.
The Command Palette is a centered bounded fraction of terminal width and height and renders its title, prompt, footer, and completion detail.
Quick Filter replaces the bottom status-bar content with its draft and inline completion suffix; its completion list, footer, and current expression error occupy a bounded popup anchored directly above that row.
Runtime/query completions without TUI category metadata retain their core detail.

### Workspace and overlays

Track navigation moves selection by row/page/endpoints; group navigation selects the first track in the previous/next section.
Mouse wheel moves selection by three tracks.
Dragging the table scrollbar maps its visual position to a selectable track.
Clicking a section header selects its first track; dragging a header edge previews a width in terminal cells and releasing it commits canonical state for the current base list.

After runtime workspace restore, `LibraryController` reads the active process-local `ViewId` and materializes that view directly.
It does not navigate by `ListId`, so multiple filtered views over one list remain distinct and construction adds no history entry.
Only a missing or unusable active view opens the All Tracks fallback.
A fallback or later plain-list navigation supplies that list's saved or recommended presentation as `NewViewDefault`; runtime view reuse still retains the exact active presentation.
A successful user presentation choice records the current base list's preference, while filtering and exact workspace attachment do not.
Reload follows the same direct materialization path; if the controller's view disappeared, it attaches a different active workspace view when available and otherwise opens All Tracks.

The list overlay renders All Tracks first and walks the shared list tree in preorder.
Children of the virtual All Tracks root retain zero terminal indentation, while every additional user-list ancestor adds two spaces.
Every saved List uses the terminal-specific `[L]` icon, and a nonempty local expression appears as detail text.
Nesting expresses derivation from a parent List; it does not introduce a Folder or List kind.

The detail overlay is a live inspector rather than a modal panel.
While it is visible, track and group navigation, wheel selection, scrollbar drag, section-header selection, column resizing, playback, seek, volume, Quick Filter, and the Command Palette all remain available against the reduced workspace geometry, and the pane follows the selection the workspace produces.
Entering text input suspends those gestures for the duration of the input without closing the inspector.
There is no track-row click target.

The list, detail, quality, output, presentation, and notification panels use their effective toggle shortcut to reopen or close that panel, and show that shortcut in the panel footer.
Help remains a modal surface closed by fixed Escape rather than by its root open shortcut.
Return activates the selected list, presentation, or output row.
Escape closes the active overlay.
Notification `x` locally suppresses the compact activity entry according to the shared activity model.

### Playback dock and seek

The single-row dock contains the Soul transport/quality control, title/artist, output badge, elapsed, bounded responsive seek rail, duration, and volume percentage.
The Soul control toggles playback on click; hover shows quality detail without opening a modal overlay.
The effective `playback.playPause` shortcut (shipped with Space first) and the Soul control pause active playback and resume paused playback even while an output-device selection is pending.
From Idle, they resume a restored sequence-owned current track; otherwise they start the selected track.
Stop is an idempotent silent no-op when playback is already Idle.

Seek press begins a shared `SeekInteraction` gesture, pointer motion publishes preview seeks, and release publishes the final seek through `PlaybackPositionViewModel`.
Release beyond the rail clamps to the rail range.
Keyboard seek asks the same view model for a clamped five-second relative change and is inert without a known positive duration.
Keyboard volume asks `VolumeViewModel` for a clamped five-percentage-point relative change, including the shared rule that raising volume clears explicit mute.

### Rendering

Panels use titled-frame chrome with one-cell horizontal body padding; the dense track workspace omits body padding.
The detail pane's titled frame is its only chrome: present cover art renders as artwork alone, with no nested title, separator, or border, and block and Kitty delivery reserve the same terminal cells.
The detail pane derives its width from the active locale's field labels plus a fixed value budget and the cover width, capped at half the terminal.
Field labels are capped at twelve cells before they are measured, as the GTK detail grid caps its key column, so a locale with long field names shortens them instead of widening the pane.
Inside that body the label column takes at most forty percent and leaves the value column at least twelve cells at the `80x24` target.
Detail rows stay on one line and are shortened by display-cell width with a trailing ellipsis rather than split or silently clipped.
Shortening cuts only between whole clusters, so a joined emoji sequence, a flag, or a combining sequence is dropped entirely rather than emitted as a fragment.
Cover art is shown only when its fixed rows, the pane chrome, and the worst-case metadata row count all fit the available main-content rows, so artwork visibility does not change as selection moves; short terminals give the rows to metadata.
The workspace lower frame edge carries list/view identity on the left and selection/count state on the right.
Selected rows and hovered controls use one centralized yellow/black/bold interactive style.

The playback Soul animation consumes shared UIModel aura/color/timing policy while terminal code chooses braille geometry.
Opening, Buffering, Playing, and Seeking keep periodic animation refresh active; elapsed-time interpolation advances only in the transport states identified as playing by `PlaybackPositionViewModel`.
Short terminals keep the dock to one row before reducing track-table height further.

## Failure and cancellation

Unavailable actions post warning notifications rather than inventing terminal-only error state; the idempotent Idle Stop exception remains silent.
Stale section/output rows are rejected and reported.
Submitting a filter calls the typed runtime view boundary before replacing terminal rows.
On Error, `LibraryController` preserves the draft, active source/view, rows, sections, and selection; `EventController` logs the failure and posts an Error notification instead of reloading.
An invalid live expression remains visible in the Quick Filter panel without posting one notification per debounce tick.
Return or Escape performs one immediate final application before the panel closes; a recoverable expression error posts one Warning, while a command-level failure posts one Error.
Replacing, closing, or destroying Quick Filter input requests stop on the pending timer; a stopped or obsolete generation cannot mutate shell or library state.
Text-input or overlay entry cancels an active seek preview by committing the current runtime elapsed value as the final stabilization point, then resets the gesture.
An interrupted column drag instead discards its preview. Overlay changes, text input, list changes, unrelated pointer presses, and teardown therefore produce no column-layout model change or save.

The effective quit shortcut (shipped as `q`), the `quit` command, Ctrl-C, and signal exit only request that the event loop end; input dispatch does not stop playback early.
Normal teardown cancels pending Quick Filter debounce, seek/scrollbar/column gestures, and cover work before persistence captures state.
Cancelling an active seek drag commits the current runtime elapsed position as its final stabilization point rather than the uncommitted preview.
Teardown then checkpoints workspace, checkpoints playback, and requests playback stop.
Frontend observers and controllers are destroyed before runtime shutdown, while `ScreenInteractive` outlives the runtime executor that borrows it.
Frame timers and executor callbacks cannot access the screen after their owning application lifetime ends.

## Persistence and versioning

TUI workspace config defaults to `<root>/.aobus/tui-workspace.yaml` and follows the workspace session contract.
The runtime uses one `ConfigStore` writer for both its `workspace` and `playback-session` groups in that file.
The independent `<root>/.aobus/tui_layout.yaml` file has one TUI writer for `trackView.columnLayouts` and `trackView.presentations`; committed model changes save both groups atomically.
Column widths in that document are terminal cells and are never converted to or from desktop pixels.
Workspace and presentation state are restored before terminal view attachment; playback observation and restore follow before the event loop.
Normal exit discards any unfinished column preview, retries a pending failed presentation checkpoint, checkpoints both runtime groups, then stops playback before runtime shutdown.

Restored workspace state includes open view configurations, exact active-view choice, and custom presentation presets; it excludes track selection and navigation history beyond the reconstructed initial point.
Restored playback state includes source/filter/order, current track and position, modes, volume, and mute without autoplay.
Restored TUI presentation state includes each list's column order, visibility, fixed cell widths or flexible weights, and preferred presentation id.
Active overlay, input draft/mode, the original Quick Filter editing draft, hover, and pointer gestures are session-local and unversioned.
The preferred output route is stored separately in the global TUI application-preference file.
The same global `<config>/tui.yaml` document supplies the `shortcuts` group over shared-plus-TUI defaults.
TUI loads that group before constructing dispatch and render owners, but has no shortcut editor and performs no ordinary keymap save; the unrelated `runtime` preference checkpoint preserves the loaded sibling group.
Exact startup paths/options and managed locations belong to the TUI and persistence references.

## Frontend observations

The detail pane remains beside the track workspace and shows a terminal cover-art representation plus selected-track fields.
Title, artist, album, display track number, and duration always appear, keeping a placeholder when the track lacks them; every other field appears only when it carries a value.
Kitty, block, automatic, and disabled cover modes are selected at startup.
On a cover change, the pane renders one compact unavailable line until asynchronous delivery completes; an older selection cannot replace the current cover.
A frame that reserves no artwork cells leaves an invalid cover box behind, which is how out-of-band Kitty paint state learns to delete a stale image.
The notification center can be opened explicitly even when compact status is not the only visible affordance.

## Implementation map

- [`App.cpp`](../../../app/tui/App.cpp) composes runtime, screen, render, controllers, and lifetime.
- [`CoverArtLoader.cpp`](../../../app/tui/CoverArtLoader.cpp) owns asynchronous selected-resource delivery and stale-result suppression; [`CoverArt.cpp`](../../../app/tui/CoverArt.cpp) owns bounded decode and terminal transforms.
- [`ShellInteractionModel.cpp`](../../../app/tui/ShellInteractionModel.cpp) owns text-input, command parsing, and overlay state.
- [`TuiKeymap.cpp`](../../../app/tui/TuiKeymap.cpp) owns stable terminal action descriptors, TUI-local defaults, the FTXUI projection whitelist, collision selection, and the immutable dispatch/hint plan.
- [`CommandCompletion.cpp`](../../../app/tui/CommandCompletion.cpp) owns command and presentation completion plus explicit filter-argument routing; [`CommandCompletionProvider.cpp`](../../../app/tui/CommandCompletionProvider.cpp) separates Command Palette and live Quick Filter providers.
- [`EventController.cpp`](../../../app/tui/EventController.cpp) owns keyboard/mouse dispatch and transient-interaction cancellation.
- [`LibraryController.cpp`](../../../app/tui/LibraryController.cpp) owns exact runtime-view attachment, row materialization, preference-aware plain-list navigation, and reload fallback.
- [`LibraryNavigation.cpp`](../../../app/tui/LibraryNavigation.cpp) flattens the shared list-tree projection into terminal rows.
- [`Render.cpp`](../../../app/tui/Render.cpp) and [`Style.cpp`](../../../app/tui/Style.cpp) own common terminal composition and styling; [`CommandPalettePanel.cpp`](../../../app/tui/CommandPalettePanel.cpp) owns command/filter completion panels, and [`StatusBar.cpp`](../../../app/tui/StatusBar.cpp) owns the Quick Filter input row.
- [`TerminalTrackColumnLayout.cpp`](../../../app/tui/TerminalTrackColumnLayout.cpp) projects shared column state into terminal cells; [`TrackTable.cpp`](../../../app/tui/TrackTable.cpp) owns track-table output; [`TuiLayoutStateStore.cpp`](../../../app/tui/TuiLayoutStateStore.cpp) owns the presentation file.
- [`PlaybackPanel.cpp`](../../../app/tui/PlaybackPanel.cpp) and [`SoulButton.cpp`](../../../app/tui/SoulButton.cpp) own the dock.

## Test map

- [`ShellInteractionModelTest.cpp`](../../../test/unit/tui/ShellInteractionModelTest.cpp) protects input modes, touched state, command/overlay state, and parsing.
- [`TuiKeymapTest.cpp`](../../../test/unit/tui/TuiKeymapTest.cpp) protects action identities, shared/local defaults, terminal aliases and omissions, collision order, unbinding, and coupled dispatch/hint selection.
- [`EventControllerTest.cpp`](../../../test/unit/tui/EventControllerTest.cpp) protects input routing, live-filter debounce/cancellation, completion acceptance, key/mouse modality, seek, teardown stabilization, overlays, resizing, and exit without early playback stop.
- [`LibraryControllerTest.cpp`](../../../test/unit/tui/LibraryControllerTest.cpp) protects exact restored-view attachment, valid empty projections, reload preservation, restored custom presets, and list-deletion recovery.
- [`TerminalTrackColumnLayoutTest.cpp`](../../../test/unit/tui/TerminalTrackColumnLayoutTest.cpp), [`TrackTableTest.cpp`](../../../test/unit/tui/TrackTableTest.cpp), and [`TuiLayoutStateStoreTest.cpp`](../../../test/unit/tui/TuiLayoutStateStoreTest.cpp) protect terminal-cell projection, sections, viewport, persisted widths, and selection.
- [`LibraryNavigationTest.cpp`](../../../test/unit/tui/LibraryNavigationTest.cpp) protects shared-tree preorder adaptation, indentation, icons, and details.
- [`RenderTest.cpp`](../../../test/unit/tui/RenderTest.cpp), [`PlaybackPanelTest.cpp`](../../../test/unit/tui/PlaybackPanelTest.cpp), and [`TuiHitRegionsTest.cpp`](../../../test/unit/tui/TuiHitRegionsTest.cpp) protect rendering and hit geometry.
- Command completion tests under [`test/unit/tui/`](../../../test/unit/tui/) protect prefix, alias, presentation, Quick-filter, and expression completion.

## Related documents

- [Presentation architecture](../../architecture/presentation.md)
- [Interactive session lifecycle architecture](../../architecture/interactive-session-lifecycle.md)
- [Track-column layout](../presentation/track-column-layout.md)
- [List-navigation tree](../presentation/list-tree.md)
- [Activity status](../presentation/activity-status.md)
- [TUI command reference](../../reference/tui/command.md)
