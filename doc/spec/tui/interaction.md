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
- The **Track Properties editor** is a full-surface modal that edits one frozen vector of captured track ids; it is not one of the overlays above.

## Invariants

- Text input is modal, and every overlay except the detail inspector is modal: workspace-only input cannot mutate the track table beneath them.
- Only one overlay is active at a time; opening another overlay replaces the detail inspector, while text input leaves it open beneath the suspended workspace.
- A surface change retires pointer gestures: opening or closing any overlay, and entering text input, cancels an active seek, scrollbar, or column drag rather than letting it finish against a layout the user did not aim at.
- Detail pane width follows locale and terminal width only. Selection, cover presence, and optional-field presence cannot change it.
- Render code writes hit regions into the one `TuiHitRegions` owner; input reads that same frame state.
- Shared list navigation handles arrows, pages, home, and end before a panel-specific selection callback.
- Fixed input, list, modal-overlay, notification, mouse, and escape protocol is resolved before configurable root actions; Ctrl-C, `:quit`, the quit shortcut, and handleable platform signals share one graceful App exit gate.
- One prepared `TuiKeymapPlan` drives root dispatch and every hint for a configurable action, so rebinding or unbinding cannot leave a hard-coded execution or display path behind.
- Selection always resolves to a track even when scrollbar geometry counts group headers.
- Explicit marks are a transient TUI set distinct from the focused row. The effective edit/publication selection is the marked ids in current view order when any marks exist, otherwise the focused track.
- The Track Properties editor captures its target ids once at open; nothing during its lifetime adds, removes, or reorders them, and changing the target set requires closing and opening a new editor.
- An open editor consumes every event that reaches it, so no root action, overlay toggle, or mouse gesture behind it can run against geometry the user cannot see.
- A submitted metadata write outlives the editor that started it; exit waits for exactly that write rather than for the editor.
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

The palette completes command names and aliases, including multi-word exact aliases such as `scan cancel`, presentation ids, and filter candidates only within an explicit `filter` argument.
A `scan` prefix may list `scan` and `scan cancel`; a trailing space after `scan` converges to `scan cancel`.
Enter on bare `select` remains an unknown command.
The Command Palette is a centered bounded fraction of terminal width and height and renders its title, prompt, footer, and completion detail; being both centered and not live, it dims the workspace it covers.
Quick Filter replaces the bottom status-bar content with its draft and inline completion suffix; its completion list, footer, and current expression error occupy a bounded popup anchored directly above that row.
Runtime/query completions without TUI category metadata retain their core detail.

### Workspace and overlays

Track navigation moves focus by row/page/endpoints; group navigation focuses the first track in the previous/next section.
Mouse wheel moves focus by three tracks.
Dragging the table scrollbar maps its visual position to a focused track.
Clicking a section header focuses its first track; dragging a header edge previews a width in terminal cells and releasing it commits canonical state for the current base list.

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

Overlays are composed over the root in one place, ordered by keyboard ownership: whichever surface answers for every key is drawn last, so no candidate below it can cover a surface that still owns input.
Each candidate is built only once those above it decline, so an overlay that is not drawn publishes no hit regions for rows nobody can click.
Only a centred modal surface dims what it covers, because it stands on its own rather than beside the control that opened it and the workspace under it is not what the user is acting on.
An anchored panel leaves the workspace lit, and Quick Filter must, because the track table behind it is the very thing the typing changes.

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
Marked track rows use a reverse-video mark surface; it pins no palette slot, so it reverses whichever pair the row already resolved.
An unfocused marked row therefore reverses the terminal's own colors and a focused marked row reverses the interactive pair, which reports mark state in both focus states without a dedicated cell.
The now-playing caret keeps its accent only on unfocused rows, because a cell that holds its own foreground over the interactive surface renders an unreadable pair.

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

`:scan` and `:rescan` start one eager `LibraryScanController` flight through `uimodel::runLibraryScanAsync`.
`:scan cancel` requests stop on that flight.
Start, cancel, retire, and completion bookkeeping run on the TUI callback executor, which is the FTXUI dispatch lane; `phase` is unsynchronized.
A start while Running posts a transient already-running notice; a start while Cancelling posts a transient cancellation-in-progress notice.
`async::OperationCancelled` is silent; a returned `LibraryScanOutcome`, including Failed, is presented even if cancellation is in progress.
Retirement suppresses late presentation.
Scan progress continues to use `ActivityStatusViewModel` observing `LibraryJobs`.

`m` toggles the focused track in the marked set.
`v` starts a visual selection anchored at the focused track, and pressing it again confirms the range into the marked set.
While a visual selection runs, every focus move re-derives the marked set as the selection's starting set plus the inclusive flattened-track range from the anchor to the focus, so the range is visible as it grows rather than only after it is committed.
Escape cancels a running visual selection and restores the marked set exactly as it was when the selection started.
A modal overlay is closed first, because it holds the keys that grow the range; the detail panel is not modal and leaves those keys to the workspace, so Escape cancels the selection and the panel stays open.
Confirming, `m`, `Shift+A`, any mark reset, and an anchor that leaves the materialized view all end the visual selection; `m` commits the current range before toggling the focused track, so neither edit is lost.
Ending a selection because its anchor disappeared keeps the rows the range had already reached; only Escape restores the starting set.
While a selection runs, the workspace footer names the mode ahead of the mark count, because the count alone does not say whether the next motion still grows the range.
`Shift+A` marks every materialized track in the current view.
`u` clears the marked set.
`j` and `k` move the focused track the same way the arrow keys do.
Marking is reported by the mark surface in every focus state.
Moving the cursor does not add or remove marks unless a visual selection is running.
A successful effective filter change, or navigation to another list/view, clears marks.
Reattaching the same view, including confirming the current List in the chooser, reconciles marks and focus rather than resetting them; only attaching a different view starts at the top.
A same-view reload or presentation change intersects marks with remaining track ids, and a still-running visual selection derives its range again from the reconciled starting set, the anchor, and the reconciled focus, because the range is defined by row order rather than by the ids it happened to cover.
A successful no-op filter keeps marks.
Mouse input still only moves focus.
`publishSelection()` publishes `selectedTrackIds()`. Playback Enter and Detail/cover remain focus-based: marks change the published selection and the status count, not which track Enter starts.

The effective edit shortcut (shipped as `e`), `:edit`, and `:properties` open one `TrackEditController` editor over `selectedTrackIds()`.
Preparation is one attempt on the callback executor: it begins a `TrackAuthoringSession` over the captured ids, then reads every field and tag count of every target from one `LibrarySnapshot` and refuses when that snapshot's revision differs from the session's bound revision.
An empty selection, a bind failure, and a revision mismatch all refuse without opening, each posting its own Warning; a bind-time missing target reports the incomplete selection rather than a generic availability refusal.
A refusal changes nothing, and a second open while an editor is already active is refused too.
Opening retires command and filter input, cancels the filter debounce and pointer gestures, rolls back a column preview, and commits a running visual range into the mark set; the Detail inspector's visibility preference is preserved rather than closed.
The range's rows stay marked because they are what the editor captured, but the anchor goes: leaving it armed would let the first motion key after the modal closes reshape those marks, and a library change under the modal would re-derive the range against rows the edit itself reordered.

The editor is composed as a centered modal overlay over the live workspace using a dimmed backdrop.
The workspace remains visible but inert behind the modal: all keyboard events and mouse gestures reach only the editor.
Cover art is neither requested nor painted while the editor is active.
The Kitty image in particular is written to the terminal outside FTXUI's cell buffer after the frame is flushed, so it would paint over the modal rather than under it; the Kitty owner therefore removes its image while an editor is active or an exit is in progress.

The editor organizes authoring into pages: `Metadata`, `Tags`, read-only `Properties`, and (for multi-track selections) `Tracks`.
`Tab` and `Shift-Tab` cycle forward and backward through available pages from any control, search input, or completion popup.
Inside the Metadata page, editing uses direct keyboard input without checkboxes.
A passive changed indicator (`*`) marks edited rows, while an active indicator (`>`) highlights the focused row.
Typing text into a field marks it for replacement; typing back the baseline value removes the pending change.
Deleting an edited field to empty marks it for explicit clear.
For mixed-value fields across multiple tracks, `Ctrl-U` triggers an explicit clear across all targets, while `Ctrl-G` restores the field to baseline or mixed preservation.
Inline numeric validation flags parsing errors and disables save while preserving the draft input.

Metadata fields supporting vocabulary completion (Artist, Album, Album Artist, Genre, Composer, Conductor, Ensemble, Work, Movement, Soloist) query the runtime `CompletionService` synchronously on the event thread.
Non-empty typing or `Ctrl-N` triggers completion candidates in an anchored popup.
`Up`/`Down` and `PageUp`/`PageDown` navigate candidates, `Enter` replaces the targeted field text via checked range replacement (`tryReplaceRange`), and `Esc` closes the completion popup without dismissing the editor.
The six-row candidate window moves only when arrow navigation leaves it; page navigation advances both selection and window by six rows, clamped at either end.
The metadata viewport follows the selected candidate while completion is open, including in a terminal too short to show the entire popup.
Every chord the popup declines -- `Ctrl-S`, `Ctrl-R`, `Ctrl-U`, `Ctrl-G`, and page switching -- closes it before the editor acts on it, so no confirmation prompt is drawn under candidates it cannot take input for.
Single-line values refuse control characters, U+2028, and U+2029 rather than sanitizing them, one `insert` call at a time; FTXUI implements no bracketed paste, so a pasted string arrives as ordinary key events with its newlines as `Return` and cannot be refused as a unit.
Caret navigation (`Left`, `Right`, `Home`, `End`) closes completion.

The `Tags` page leads every row with a three-state box showing what all captured targets would carry after a submission: `[x]` for every target, `[ ]` for none, and `[~]` for a tag only some of them carry.
A pending intent moves the box now and colours it, and names the change beside the tag as `Add` or `Remove`.
The box says nothing about where a partly carried tag started, so those rows and only those rows also carry the `carried/captured` fraction.
Library tags no captured track carries follow the selection's own, needing no heading because an empty box already says no captured target carries the tag, and are capped at the 50 highest-frequency suggestions; a truncated list reports how many it left out.
Tags already marked by the draft and exact query matches are exempt from that cap, so the existing tag remains reachable even when many higher-frequency suggestions contain its name.
The page has no second mode: a query field above the list is always live, so any printable key, `Backspace`, `Delete`, and caret navigation edit the query while `Up`/`Down` and `PageUp`/`PageDown` move the selection.
The query filters the whole library vocabulary by Unicode case-insensitive substring using `makeUtf8CaselessKey`; it is never part of the draft, and `Esc` drops a non-empty query before it means anything about the editor.
Matching keys normalize to NFC and apply Unicode default case folding; accepted existing tags retain their stored spelling, and new names are normalized to NFC without changing case.
A decomposed or case-variant spelling therefore finds the existing tag instead of offering another row.
A query wider than the modal scrolls horizontally to keep the caret visible rather than shrinking, matching the metadata inputs, and stays one line tall.
`Enter` advances the selected tag through the intents that would write something: `AddToAll` then back to baseline when no target carries it, `RemoveFromAll` then back to baseline when every target does, and `AddToAll`, `RemoveFromAll`, baseline when only some do.
A query naming no existing tag offers creation on a trailing row, so `Enter` prefers an existing match whenever the query found one; a created tag joins the captured tags rather than the suggestions.
Empty or ASCII-whitespace-only queries never offer creation; surrounding spaces in a nonblank name remain part of its spelling.
Committing an intent drops the query and keeps the selection on that tag, and `Ctrl-G` restores the selected tag to baseline.

The retained session's invalidation signal is observed on the callback executor and marks a Ready editor Stale, which disables Save while preserving the raw draft.
Any effective library commit can invalidate the session, including one unrelated to the edited tracks.
An invalidation arriving while a write is in flight is ignored, because that write reconciles its own binding and reports its own terminal result.
`Ctrl-R` re-runs the same one-attempt preparation against the entire captured id vector and replaces the baseline, raw inputs, and session only on success; on failure the previous draft and its diagnostic remain, and no draft is ever reattached to a fresh session automatically.

`Ctrl-S` compiles and submits one unified `TrackPropertiesPatch` containing metadata edits and tag changes (`tagsToAdd`, `tagsToRemove`) through the retained session via `submitPropertiesAsync()`.
Both entry into the lazy session submission and terminal-result handling run on the callback executor, alongside session invalidation.
Changed targets across metadata and tags are deduplicated into a single mutation count.
Applied and NoOp close the editor and post the applied or no-change result; a reply with fewer change records than targets is not partial failure.
Busy preserves the draft and binding and re-enables Save only while the session is still current, otherwise it becomes Stale.
The retryable Busy diagnostic clears on the next keyboard event so normal shortcuts become visible again.
Confirmation and diagnostic text wraps to the modal width; recovery shortcuts occupy their own row so a long translation cannot truncate the key names.
Stale and Unavailable preserve the draft and offer Reload or Close, and an operational `Result` error does the same with its own message.
If a submission is cancelled while the editor remains active, it becomes Stale and preserves the draft for Reload or Close.
Pending-submission bookkeeping clears before any presentation work, so a failing presentation cannot leave exit waiting for a write that already landed.
Closing the editor during a submission keeps the session alive until that write settles; controller retirement suppresses late presentation but never cancels the write, and the destructor only drops the editor, the invalidation observer, and its output callbacks without dispatching or presenting anything.

The effective quit shortcut (shipped as `q`), the `quit` command, terminal Ctrl-C, and handleable platform signals (POSIX SIGINT/SIGTERM/SIGHUP; Windows Ctrl-C/Ctrl-Break/close) request one App-owned `ExitController`.
The first request asks once whether a submitted metadata write is still settling, retires scan and editor presentation and transient input, then posts loop exit.
A pending write instead moves the gate to a waiting phase: the status row is replaced by a waiting message, ordinary input is consumed before it reaches the editor or the workspace, and either the write settling or a second exit request posts loop exit.
Input dispatch does not stop playback early.
Normal teardown cancels pending Quick Filter debounce, seek/scrollbar/column gestures, cover work, and scan presentation before persistence captures state.
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
- [`CommandCompletion.cpp`](../../../app/tui/CommandCompletion.cpp) owns command and presentation completion plus explicit filter-argument routing; [`App.cpp`](../../../app/tui/App.cpp) composes the Command Palette and live Quick Filter callbacks.
- [`EventController.cpp`](../../../app/tui/EventController.cpp) owns keyboard/mouse dispatch and transient-interaction cancellation, and forwards graceful exit without owning `ScreenInteractive`.
- [`LibraryScanController.cpp`](../../../app/tui/LibraryScanController.cpp) owns the single restartable scan task.
- [`ExitController.cpp`](../../../app/tui/ExitController.cpp) owns the idempotent graceful-exit gate; [`SignalExitWatcherPosix.cpp`](../../../app/tui/SignalExitWatcherPosix.cpp) and [`SignalExitWatcherWindows.cpp`](../../../app/tui/SignalExitWatcherWindows.cpp) post those requests from platform signals.
- [`TrackEditController.cpp`](../../../app/tui/TrackEditController.cpp) owns coherent preparation, the retained authoring session and its invalidation observer, submission, and retirement; [`TrackPropertiesEditor.cpp`](../../../app/tui/TrackPropertiesEditor.cpp) owns the editor's own focus, keys, draft state, and rendering.
- [`LibraryController.cpp`](../../../app/tui/LibraryController.cpp) owns exact runtime-view attachment, row materialization, preference-aware plain-list navigation, and reload fallback.
- [`LibraryNavigation.cpp`](../../../app/tui/LibraryNavigation.cpp) flattens the shared list-tree projection into terminal rows.
- [`Render.cpp`](../../../app/tui/Render.cpp) and [`Style.cpp`](../../../app/tui/Style.cpp) own common terminal composition and styling; [`CommandPalettePanel.cpp`](../../../app/tui/CommandPalettePanel.cpp) owns command/filter completion panels, and [`StatusBar.cpp`](../../../app/tui/StatusBar.cpp) owns the Quick Filter input row.
- [`TerminalTrackColumnLayout.cpp`](../../../app/tui/TerminalTrackColumnLayout.cpp) projects shared column state into terminal cells; [`TrackTable.cpp`](../../../app/tui/TrackTable.cpp) owns track-table output; [`TuiLayoutStateStore.cpp`](../../../app/tui/TuiLayoutStateStore.cpp) owns the presentation file.
- [`PlaybackPanel.cpp`](../../../app/tui/PlaybackPanel.cpp) and [`SoulButton.cpp`](../../../app/tui/SoulButton.cpp) own the dock.

## Test map

- [`ShellInteractionModelTest.cpp`](../../../test/unit/tui/ShellInteractionModelTest.cpp) protects input modes, touched state, command/overlay state, and parsing.
- [`TuiKeymapTest.cpp`](../../../test/unit/tui/TuiKeymapTest.cpp) protects action identities, shared/local defaults, terminal aliases and omissions, collision order, unbinding, and coupled dispatch/hint selection.
- [`EventControllerTest.cpp`](../../../test/unit/tui/EventControllerTest.cpp) protects input routing, live-filter debounce/cancellation, completion acceptance, key/mouse modality, seek, teardown stabilization, overlays, resizing, scan commands, selection commands, and exit without early playback stop.
- [`ExitControllerTest.cpp`](../../../test/unit/tui/ExitControllerTest.cpp) protects exit phase-before-output, reentrancy, and one exit publication.
- [`LibraryScanControllerTest.cpp`](../../../test/unit/tui/LibraryScanControllerTest.cpp) protects single-flight scan cancellation, late-result suppression, and the production eager-scan binding.
- [`TrackEditControllerTest.cpp`](../../../test/unit/tui/TrackEditControllerTest.cpp) protects open refusal, single-editor exclusivity, one patch across every captured target, invalidation staleness, reload rebinding, and a submission settling after its editor and controller retired.
- [`TrackPropertiesEditorTest.cpp`](../../../test/unit/tui/TrackPropertiesEditorTest.cpp) protects focus order, Apply intent, mixed-value placeholders, patch construction, confirmations, and submission-state rendering.
- [`TrackPropertiesEditorCompletionTest.cpp`](../../../test/unit/tui/TrackPropertiesEditorCompletionTest.cpp) protects candidate acceptance, stable paging, popup dismissal, and selected-candidate visibility in short terminals.
- [`TrackPropertiesEditorTagsTest.cpp`](../../../test/unit/tui/TrackPropertiesEditorTagsTest.cpp) protects tag intent, Unicode matching, suggestion caps, and query editing.
- [`TuiSignalProbeTest.cpp`](../../../test/unit/tui/TuiSignalProbeTest.cpp) drives [`ao_tui_signal_probe`](../../../test/fatal/TuiSignalProbeScenario.cpp) to protect watcher signal routing and previous-handler restoration outside the ordinary unit-test process.
- [`LibraryControllerTest.cpp`](../../../test/unit/tui/LibraryControllerTest.cpp) protects exact restored-view attachment, valid empty projections, reload preservation, restored custom presets, list-deletion recovery, and mark/range/select-all reconciliation.
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
