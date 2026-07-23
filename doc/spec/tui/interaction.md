---
id: tui.interaction
type: spec
status: current
domain: presentation
summary: Defines TUI shell modes, panels, selection, mouse dispatch, playback dock, seek rail, command completion, notification, and rendering behavior.
---
# TUI interaction specification

## Scope

This specification owns the terminal frontend's shell and interaction behavior.
It defines workspace structure, modal command/overlay state, keyboard and mouse routing, panel mechanics, playback dock and seek rail, command completion, notifications, selection, and terminal styling.
Exact startup options, keys, commands, and aliases belong to the [TUI command reference](../../reference/tui/command.md).

## Code boundary

TUI code under `app/tui/` owns FTXUI elements, terminal geometry, hit regions, input dispatch, frame timing, terminal cover rendering, and TUI-local shell state.
It consumes `AppRuntime` and shared UIModel policies for presentation, seek gestures, status, output, quality, column widths, and soul animation.

`LibraryController` adapts runtime workspace/views into terminal rows but does not become library storage or playback authority.
Its list chooser consumes the shared [list-navigation tree](../presentation/list-tree.md) instead of deriving parent relationships or sibling order.
`EventController` translates terminal events into runtime/UIModel commands.
`CoverArtLoader` owns one cancellable selected-resource request; byte reads and cover transforms run off the screen executor and publish only for the current resource generation.

## Terminology

- **Command mode** is the text-entry state entered by `/` or `:`.
- An **overlay** is one of list, detail, quality, output, presentation, notification, or help panels.
- A **hit region** is a rendered FTXUI box retained for the next mouse-dispatch pass.
- The **seek rail** is only the reflected timeline/thumb segment, excluding elapsed/duration text.
- A **visual row** includes group headers as well as selectable track rows.

## Invariants

- Command mode and overlays are modal: workspace-only input cannot mutate the track table beneath them.
- Only one overlay is active at a time.
- Render code writes hit regions into the one `TuiHitRegions` owner; input reads that same frame state.
- Shared list navigation handles arrows, pages, home, and end before a panel-specific selection callback.
- Selection always resolves to a track even when scrollbar geometry counts group headers.
- Equivalent playback, presentation, filtering, notification, and output actions use shared runtime/UIModel authorities.
- The list chooser preserves the shared list-tree parent recovery and sibling order; TUI code owns only terminal flattening and decoration.
- Soul/Space transport toggles and ordinary stop requests use `PlaybackCommandSurface`; explicit selected-track activation remains a distinct view-based sequence command.
- Active seek drag is canceled when command mode or an overlay becomes active.
- A zero-duration timeline rejects pointer and relative-keyboard seek.
- Column drag changes only TUI session-local widths.
- Ordinary terminal styling inherits the terminal background; semantic roles add accents without painting broad application backgrounds.

## State model

`ShellInteractionModel` retains command-active state, UTF-8 draft text, completion result/selection, and active overlay.
`EventController` retains pointer drags for seek, scrollbar, and column resize plus hover state.
`LibraryController` retains active runtime view, terminal row snapshot, selected track index, sections, filter draft, and presentation adaptation.

The command palette completion result carries a replacement range, ranked items, display text, insertion text, and detail.
Filter drafts delegate to the shared UIModel track-filter completer, which selects live Quick-filter values or structured expression candidates according to the same boundary as GTK.
Presentation contexts add built-in and custom preset ids.

## Commands and transitions

### Command mode

`/` or `:` opens command mode.
Text appends as UTF-8, backspace removes one complete code point, arrows move completion selection, Tab applies the selected completion, Return submits, and Escape cancels.
Known prefixes/aliases produce typed actions; otherwise the submitted text becomes a quick filter.
The palette completes command names and aliases, presentation ids, structured expression tokens, and live Quick-filter values from titles, the common search fields, and tags.
Applying a live value replaces the active filter term with one safely quoted term.

The palette is a centered bounded fraction of terminal width/height and renders category, command text, shortcut, and detail.
Runtime/query completions without TUI category metadata retain their core detail.

### Workspace and overlays

Track navigation moves selection by row/page/endpoints; group navigation selects the first track in the previous/next section.
Mouse wheel moves selection by three tracks.
Dragging the table scrollbar maps its visual position to a selectable track.
Clicking a section header selects its first track; dragging a header edge installs a session-local width override.

The list overlay renders All Tracks first and walks the shared list tree in preorder.
Children of the virtual All Tracks root retain zero terminal indentation, while every additional user-list ancestor adds two spaces.
Folder, Manual, and Smart rows receive terminal-specific `[+]`, `[#]`, and `[?]` icons, and Smart List expressions appear as detail text.

Overlay toggle keys reopen/close their own panel.
Return activates the selected list, presentation, or output row.
Escape closes the active overlay.
Notification `x` locally suppresses the compact activity entry according to the shared activity model.

### Playback dock and seek

The single-row dock contains the Soul transport/quality control, title/artist, output badge, elapsed, bounded responsive seek rail, duration, and volume percentage.
The Soul control toggles playback on click; hover shows quality detail without opening a modal overlay.
Space and the Soul control pause active playback and resume paused playback even while an output-device selection is pending.
From Idle, they resume a restored sequence-owned current track; otherwise they start the selected track.
Stop is an idempotent silent no-op when playback is already Idle.

Seek press begins a shared `SeekSliderInteractionModel` gesture, pointer motion publishes preview seeks, and release publishes the final seek through `SeekViewModel`.
Release beyond the rail clamps to the rail range.
Keyboard seek asks the same view model for a clamped five-second relative change and is inert without a known positive duration.
Keyboard volume asks `VolumeViewModel` for a clamped five-percentage-point relative change, including the shared rule that raising volume clears explicit mute.

### Rendering

Panels use titled-frame chrome with one-cell horizontal body padding; the dense track workspace omits body padding.
The workspace lower frame edge carries list/view identity on the left and selection/count state on the right.
Selected rows and hovered controls use one centralized yellow/black/bold interactive style.

The playback Soul animation consumes shared UIModel aura/color/timing policy while terminal code chooses braille geometry.
Opening, Buffering, Playing, and Seeking keep periodic animation refresh active; elapsed-time interpolation advances only in the transport states identified as playing by `PlaybackTimeViewModel`.
Short terminals keep the dock to one row before reducing track-table height further.

## Failure and cancellation

Unavailable actions post warning notifications rather than inventing terminal-only error state; the idempotent Idle Stop exception remains silent.
Stale section/output rows are rejected and reported.
Command/overlay entry cancels an active seek preview by committing the current runtime elapsed value as the final stabilization point, then resets the gesture.

Signal exit requests leave through the TUI's exit watcher and normal runtime teardown.
Frame timers and executor callbacks cannot access the screen after their owning application lifetime ends.

## Persistence and versioning

TUI workspace config defaults to `<root>/.aobus/tui-workspace.yaml` and follows the workspace session contract.
Column overrides, active overlay, command draft, hover, and pointer gestures are session-local and unversioned.
Exact startup paths/options belong to the TUI reference.

## Frontend observations

The detail pane remains beside the track workspace and shows a terminal cover-art representation plus selected-track fields.
Kitty, block, automatic, and disabled cover modes are selected at startup.
On a cover change, the pane renders its empty placeholder until asynchronous delivery completes; an older selection cannot replace the current cover.
The notification center can be opened explicitly even when compact status is not the only visible affordance.

## Implementation map

- [`App.cpp`](../../../app/tui/App.cpp) composes runtime, screen, render, controllers, and lifetime.
- [`CoverArtLoader.cpp`](../../../app/tui/CoverArtLoader.cpp) owns asynchronous selected-resource delivery and stale-result suppression; [`CoverArt.cpp`](../../../app/tui/CoverArt.cpp) owns bounded decode and terminal transforms.
- [`ShellInteractionModel.cpp`](../../../app/tui/ShellInteractionModel.cpp) owns command and overlay state.
- [`CommandCompletion.cpp`](../../../app/tui/CommandCompletion.cpp) owns command, presentation, and shared filter-completion routing.
- [`EventController.cpp`](../../../app/tui/EventController.cpp) owns keyboard/mouse dispatch.
- [`LibraryNavigation.cpp`](../../../app/tui/LibraryNavigation.cpp) flattens the shared list-tree projection into terminal rows.
- [`Render.cpp`](../../../app/tui/Render.cpp), [`Style.cpp`](../../../app/tui/Style.cpp), and [`TrackTable.cpp`](../../../app/tui/TrackTable.cpp) own terminal output.
- [`PlaybackPanel.cpp`](../../../app/tui/PlaybackPanel.cpp) and [`SoulButton.cpp`](../../../app/tui/SoulButton.cpp) own the dock.

## Test map

- [`ShellInteractionModelTest.cpp`](../../../test/unit/tui/ShellInteractionModelTest.cpp) protects command/overlay state and parsing.
- [`EventControllerTest.cpp`](../../../test/unit/tui/EventControllerTest.cpp) protects key/mouse routing, modality, seek, overlays, and resizing.
- [`TrackTableTest.cpp`](../../../test/unit/tui/TrackTableTest.cpp) protects sections, viewport, widths, and selection.
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
