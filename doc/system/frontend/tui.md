---
id: tui.interaction
---

# TUI interaction specification

## Scope

This specification owns the terminal frontend's shell and interaction behavior.
It defines workspace structure, modal text-input and overlay state, keyboard and mouse routing, panel mechanics, playback dock and seek rail, completion, notifications, selection, and terminal styling.
The independent [TUI track-authoring specification](tui-track-authoring.md) owns the exact Properties and tags-only editor workflow after entry from this shell, and the [TUI List-authoring specification](tui-list-authoring.md) owns the Saved-List definition editor and deletion flow after the same entry.
Exact startup options, keys, commands, and aliases belong to the [TUI command reference](../../reference/tui/command.md).

## Settings and live publication

The mouse preference updates terminal tracking by reinstalling FTXUI terminal hooks through `WithRestoredIO`; the component graph and playback runtime remain alive. The App-owned signal watcher retires before that reinstall and is recreated afterward, preserving its precedence over FTXUI signal handlers and the normal exit checkpoint path. The applied preference gates mouse events before dispatch to Settings, Track Properties, or workspace controls.

Settings consumes input before root shortcuts, cannot coexist with the track editor, and cancels unfinished pointer/input interactions when opened. Its General, Appearance, Interaction, and Keyboard pages edit only global preferences. Per-list presentation and column-layout state stay outside Settings.

Settings tabs and rows are clickable; value arrows adjust preferences, a language row confirms the language, and a shortcut chord starts replacement capture. Footer actions use the same fixed key protocol, while wheel input only moves the selection.

Preference and keymap candidates save before live publication. A failed candidate remains visible for retry or discard, while effective behavior retains its previous value.

A language change constructs the new catalog and ICU ordering policy, saves the preference, then publishes on the callback executor. Cached navigation, row labels, output labels, and activity projections refresh before the next frame. Browsing projections rebuild sort keys and completion materializations invalidate. Existing transient playback projections retain shared ownership of their previous policy and order; new projections use the new policy. Playback, workspace identity, focus, marks, and tasks remain alive.

### Terminal title

The terminal-title preference uses the existing scalar format language.
Settings requests one compilation and preview as the draft changes, using the current playing track; draft changes never publish a terminal title.
The main loop refreshes an active valid preview after runtime publication, while rendering only reads prepared text and never compiles or opens a snapshot.
The title row opens with Enter or its mouse value control, and an invalid draft shows its diagnostic without offering Save.
Enter persists a valid draft, Escape cancels, and persistence failures retain the existing retry/discard flow.
An empty expression disables ownership.
No active or readable playing track, or an empty formatted result, falls back to Soul alone when enabled, otherwise `Aobus`.

**Refreshing the text.** The published title and the Settings preview each own a `TerminalTitleFormatter` that caches its compiled plan and playing-track text.
Applied library changes invalidate each formatter's text on the callback executor. When a read is needed, the runtime snapshot reads only the playing track, including dictionary-backed and custom fields, regardless of list selection or size.
Unchanged playback ticks neither open a snapshot nor reload track data nor evaluate a plan, even with the preview open. With no playing track, even a library publication requires no snapshot.

**Writing the title.** Only changed output produces OSC 2. The formatter replaces terminal controls and line breaks with spaces and limits the payload to 512 terminal cells without splitting Unicode text.
For Soul animation alone, the title owner writes the latest sampled frame at intervals of at least 200 milliseconds. Playing-track, formatted metadata, format preference, transport, Soul visibility, and reduced-motion changes bypass that interval.

**Restoring the shell title.** The first write for each acquisition pushes the window title onto the xterm title stack. Disabling the title or exiting normally or by a handled signal attempts one pop while the owner still holds the title, without waiting for animation.
A sink that returns `false` or throws `std::system_error` may already have delivered the escape. The owner makes at most one restoration attempt and retires output; teardown does not retry the pop. This I/O failure neither escapes through the loop nor skips session checkpoints. Other callback exceptions remain programming failures.
A terminal without title-stack support may retain the last application title until the shell replaces it.

The optional Soul prefix shares glyph selection and motion samples with the playback button.
The title owner caches the formatted track text separately from the composed title, so glyph changes never cause format reevaluation.
Unchanged track text and Soul input reuse the composed title without sanitizing or measuring it again.
New Soul input is sanitized independently, then composition enforces the total cell budget; malformed Soul input does not discard valid track text.
Braille blank characters preserve the three-cell canvas when terminals trim leading or trailing ASCII spaces.
The prefix respects paused and reduced-motion state, uses the existing playback refresh, and does not add timers or terminal-specific configuration.

## Code boundary

TUI code under `app/tui/` owns FTXUI elements, terminal geometry, hit regions, input dispatch, frame timing, terminal cover rendering, and TUI-local shell state.
It consumes `AppRuntime` and shared UIModel policies for presentation, seek gestures, status, output, quality, column widths, soul animation, and neutral keymaps.

`LibraryController` adapts runtime workspace/views into terminal rows but does not become library storage or playback authority.
Its List navigation pane consumes the shared [list-navigation tree](../presentation/list-tree.md) instead of deriving parent relationships or sibling order.
`KeymapPlan` is a replaceable frontend projection from an effective neutral keymap to executable FTXUI events and display chords.
`EventController` translates terminal events from that plan or from fixed scoped protocol into runtime/UIModel commands.
On supported Linux builds, `App` composes the shared [`ao::systemmedia` MPRIS adapter](mpris.md) after `EventController` so external commands borrow the same selection-aware `PlaybackActions` and callback executor.
The adapter lives under `app/systemmedia/linux/`; it adds no GTK dependency to the TUI and no native dependency to runtime, Core, or UIModel.
`--system-media=off` skips construction of its bridge, private thread, and artwork interests, while `auto` degrades to inert when support or a usable session bus is absent.
`CoverArtLoader` owns one cancellable selection-settle window and one cancellable selected-resource request; byte reads and cover transforms run off the screen executor and publish only for the current resource generation.

## Terminology

- **Quick Filter input** is the live filter editor entered by its effective shortcut (shipped as `/`).
- **Command Palette input** is the command editor entered by its effective shortcut (shipped as `:`).
- **Text input** means either of those mutually exclusive shell modes.
- An **overlay** is a temporary quality, output, presentation, notification, help, Go to, or Lists popover.
- A **modal** overlay blocks workspace input beneath it. Lists and Detail sidebars belong to the workspace and remain visible beneath overlays.
- A **hit region** is a rendered FTXUI box retained for the next mouse-dispatch pass.
- The **seek rail** is only the reflected timeline/thumb segment, excluding elapsed/duration text.
- A **visual row** includes group headers as well as selectable track rows.
- The **Track Properties editor** is the exclusive-input modal entered from the workspace; its authoring workflow belongs to the [TUI track-authoring specification](tui-track-authoring.md), and it is not one of the overlays above.
- The **List authoring surfaces** are the exclusive-input Saved-List editor and delete confirmation owned by the [TUI List-authoring specification](tui-list-authoring.md); they are not overlays either.

## Invariants

- Text input and every overlay are modal: workspace-only input cannot mutate the track table beneath them.
- Only one overlay is active at a time. Detail visibility and scrolling are independent: overlays and text input leave it open beneath the suspended workspace.
- A surface change retires pointer gestures: opening or closing any overlay, and entering text input, cancels an active seek, scrollbar, or column drag rather than letting it finish against a layout the user did not aim at.
- Detail pane width follows its saved request, constrained by terminal width; automatic width follows the locale. Selection, cover presence, and optional-field presence cannot change it.
- Render code writes hit regions into the one `HitRegions` owner; input reads that same frame state.
- Shared list navigation handles arrows, pages, home, and end before a panel-specific selection callback.
- Fixed input, list, modal-overlay, notification, mouse, and escape protocol is resolved before configurable root actions; Ctrl-C, `:quit`, the quit shortcut, and handleable platform signals share one graceful App exit gate.
- One prepared `KeymapPlan` drives root dispatch and every hint for a configurable action, so rebinding or unbinding cannot leave a hard-coded execution or display path behind.
- Selection always resolves to a track even when scrollbar geometry counts group headers.
- Explicit marks are a transient TUI set distinct from the focused row. The effective edit/publication selection is the marked ids in current view order when any marks exist, otherwise the focused track.
- An open editor consumes every event that reaches it, so no root action, overlay toggle, or mouse gesture behind it can run against geometry the user cannot see.
- Equivalent playback, presentation, filtering, notification, and output actions use shared runtime/UIModel authorities.
- List navigation preserves shared parent recovery and sibling order; its frontend model owns cursor identity, expansion, local search, and visible rows.
- Startup attaches the exact active runtime view restored by `WorkspaceService`; a valid empty projection does not cause replacement navigation.
- Reload materializes the same active view id and preserves its filter, presentation, grouping, sorting, history identity, filter draft, selected track when that track remains visible, and independent List cursor and presentation-picker highlight.
- Play/pause, stop, sequence previous/next, and keyboard shuffle/repeat actions use `PlaybackActions`; the mouse mode preset applies its shuffle/repeat pair through `PlaybackCommands`, and explicit selected-track activation remains a distinct view-based sequence command.
- A modal surface arriving mid-gesture ends it: a pointer drag cannot be continued while text input or a modal overlay owns the workspace.
- A zero-duration timeline rejects pointer and relative-keyboard seek.
- Column drag previews a per-list terminal-cell layout; only normal release commits it, while any interruption rolls the preview back.
- Ordinary terminal styling inherits the terminal background; semantic roles add accents without painting broad application backgrounds.

Reveal uses the shared playback reveal request. It focuses the subject in the current view if present; otherwise it prefers a matching open source view and falls back to an unfiltered All Tracks view. It verifies membership, finishes any visual range before moving focus, leaves prior filters and saved List predicates intact, and reports the outcome. The previous live filter/presentation is checkpointed in workspace history; `:back` and `:forward` traverse that history without starting playback.

Browse overlays admit only play/pause, stop, sequence navigation, mode toggles, seek, and volume after local navigation/activation/toggle handling. Search, shell input, Settings, and editors retain exclusive input ownership. Help closes on its effective Help shortcut as well as Escape.

### Playback modes

The playback bar's single mode button cycles through `SEQ-` → `SEQ*` → `SHF-` → `SHF*` → `SEQ1` → `SEQ-`, using the [locale-neutral mode codes](../../reference/tui/command.md#playback-mode-codes).
With mouse control enabled, no foreground surface owning the click, and playback ready, a left press applies the next preset through playback commands without opening a menu. When playback is unavailable, the modes remain unchanged and a warning is posted.
Keyboard shuffle and repeat remain independent and report localized status even when turning a mode off. If they select shuffle with repeat-one, the button shows `SHF1`, its hover names both preferences, and the next click returns to `SEQ-`.
An unrecognized runtime mode keeps its space but has no click or hover target; a stale click cannot change it to a recognized mode.

The dock keeps the mode and output targets stationary through ordinary ticks, volume changes, and muting when duration, backend, locale, and terminal width stay the same:

| Control         | Width budget                                                                                                                                                                                                                   |
| --------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Mode and output | The mode occupies six cells in every mode and locale. Its entire padded target highlights on hover. The two targets touch without overlapping; each contributes one padding cell between the labels, with no extra separator.  |
| Elapsed time    | Reserve the width of the known duration; until it resolves, use the compact zero-time label.                                                                                                                                   |
| Volume          | Reserve the full localized numeric volume and, where space permits, the mute label. Preserve readable title space and a seek cell before allocating extra width to mute; ellipsize a longer mute label within the stable slot. |

Hover replaces ordinary workspace hints with the localized current mode and click destination, right-aligned. When both do not fit, the current mode takes priority. The hover status omits activity if it cannot fit four frame cells and one content cell. Active input, visual selection, popovers, and keyboard panel resizing keep their own hints.

After painting, the controller clears retained hover if the last pointer position no longer hits the button. Duration resolution, a track or backend change, or resizing can thus restore ordinary hints without pointer motion. This check neither revives hover cancelled by a key nor interrupts a drag.

## State model

`ShellInteractionModel` retains an explicit `None`, Quick Filter, or Command Palette input mode, UTF-8 draft text, whether that draft has been edited, completion result/selection, and active overlay.
`EventController` retains pointer drags for seek, scrollbar, and column resize plus hover state.
It also retains one cancellable generation-checked Quick Filter debounce task; all shell and library access occurs after resumption on the callback executor.
`LibraryController` retains active runtime view, terminal row snapshot, selected track index, sections, applied filter draft and error, and presentation adaptation.
The composition root retains one shared presentation catalog, one per-list presentation-preference model, one per-list column-layout model, one frontend-local store writer for the selected library, and one replaceable TUI keymap plan loaded from the global application store.

Each input mode's completion result carries a replacement range, ranked items, display text, insertion text, and detail.
Quick Filter drafts delegate directly to the shared UIModel track-filter completer, which selects live values or structured expression candidates according to the same boundary as GTK.
Command Palette drafts search actions through their localized labels and command aliases; only the explicit `filter` command delegates its argument to the filter completer.
Presentation contexts add built-in and custom preset ids.

## Commands and transitions

### Quick Filter input

The effective `tui.library.openQuickFilter` shortcut (shipped as `/`) opens an empty Quick Filter draft without clearing or copying the currently applied filter.
Until the user edits that draft, Escape closes the input without changing the applied filter, while Return confirms the empty draft and clears the filter.
After an edit, the draft is applied after 200 milliseconds without another edit.
Every further edit or accepted completion cancels and replaces that pending generation.

Both shell inputs use `TextFieldModel`: printable UTF-8 inserts at the caret; Left/Right and Backspace/Delete operate on extended grapheme clusters. Home/End or Ctrl+A/E move to the line boundaries. Alt+B/F or Ctrl+Left/Right move across space-delimited words; Ctrl+W removes the preceding word. Ctrl+U/K remove text before/after the caret in every text field, including local search and metadata. Metadata Ctrl+D separately records explicit whole-field clearing, including mixed values. Clicking the input positions its caret. Completion receives that byte position and replaces its returned range without discarding the surrounding text; caret-only movement refreshes suggestions without rescheduling the filter debounce.
Up/Down cycles completion selection, and Page Up/Page Down moves it by the rendered completion viewport.
Ctrl+P/N recalls older/newer entries from separate command and Quick Filter histories, including the unfinished draft when moving past the newest entry. Each history holds at most 50 distinct nonempty entries for this session. Successful commands and closed Quick Filter drafts enter history; command cancellation does not.
Tab applies the selected completion, keeps the input open, refreshes completion, and schedules the resulting draft for live filtering.
Return first applies the selected completion when one exists, synchronously applies the resulting draft, and closes the input; confirming an untouched empty draft clears the filter.
Escape ignores the selected completion, synchronously applies the literal edited draft, and closes the input; an untouched draft preserves the existing filter.
Applying a live value replaces the active filter term with one safely quoted term.

### Command Palette input

The effective `tui.shell.openCommandPalette` shortcut (shipped as `:`) opens an empty Command Palette draft.
Text editing and completion navigation use the same grapheme, arrow, page, and Tab rules as Quick Filter input, but no debounce or live filtering occurs.
Return executes a complete typed command unchanged unless the user explicitly navigated the candidate list. For an incomplete command, a search query, or a deliberately selected candidate, Return applies the highlighted candidate and executes the resulting complete command. An argument prefix stays open for further input. An unmatched nonempty draft remains open and reports a warning; an empty draft without candidates closes.
Escape cancels the command draft and closes the palette.

The modes intentionally assign different submission semantics to Return and Escape.
Quick Filter is a live value editor, so Return accepts its highlighted value while Escape preserves the literal text already typed. Command Palette input has no live effect; Escape cancels it. Its exact-command priority preserves explicit intent while search candidates remain directly activatable.

A left-button press outside both the input panel and its text field follows that input mode's Escape transition and is consumed. In Quick Filter this keeps literal edited text, preserves an untouched filter, and cancels pending debounce; in Command Palette it cancels the draft. Clicking the Quick Filter status-row input remains inside the active surface.

Clicking a completion accepts that exact candidate through the Tab protocol. In Command Palette, a resulting complete command also executes; argument prefixes stay open. Quick Filter candidates leave live input open. Wheel input moves the highlighted candidate.

The palette displays one localized row per action. Canonical spellings provide insertion text; every alias participates in matching without creating another row. Matching uses Unicode case-insensitive exact, prefix, and substring ranks. ASCII abbreviations of at most four characters also support subsequence matching when no stronger match exists. Presentation ids complete after their command prefix, and filter candidates occur only within an explicit `filter` argument.
A `scan` query may show `scan` and `scan cancel`; Return on a literal `scan` still starts a scan unless the user navigated to cancellation. Bare `select` activates the highlighted selection action.
The Command Palette is a centered bounded fraction of terminal width and height and renders its title, prompt, footer, and completion detail; being both centered and not live, it dims the workspace it covers.
Quick Filter replaces the bottom status-bar content with its draft and actual caret; its completion list, footer, and current expression error occupy a bounded popup anchored directly above that row.
Runtime/query completions without TUI category metadata retain their core detail.

The Quick Filter footer reflects the input transition: empty text advertises clear/preserve, text without candidates advertises literal application, and candidates advertise acceptance/completion. An empty completion result is not a track-search failure. On narrow panels the [footer width budget](#status-hints-at-narrow-widths) gives the transition hint priority over the history reminder.

### Workspace and overlays

Empty track-table copy is selected from the active runtime view and its filter error, not an unsubmitted input draft. Invalid filters, valid filters without matches, empty Lists, and unfiltered empty All Tracks each have their own explanation; only the last offers the existing `:scan` command. The table wraps the explanation within its available width.

#### Status hints at narrow widths

The one-row workspace status bar keeps activity and the current filter on the left, with operation hints at the right. It admits whole shortcut chips, reserving Settings and Help, then the active filter and its clear action before secondary shortcuts. An ellipsis marks shortened filter text; `!` marks an invalid draft.

| Surface                   | Width constraint                   | What remains visible                                                                                                                                                                                 |
| ------------------------- | ---------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Quick Filter footer       | Panel narrower than 80 columns     | Keep the transition hint; omit the history reminder.                                                                                                                                                 |
| Filtered workspace status | Terminal narrower than 100 columns | Omit informational activity text. For active work, a warning, or an error, show a clickable Notifications indicator only if at least 16 workspace columns remain after the entry points and spacing. |

Hidden activity and Settings controls have no hit regions. With Tracks focused, the visual-range hints show the effective keep shortcut when bound and fixed Escape to cancel, even with Detail visible. Explicit Detail focus shows section hints instead.

#### Operate the track workspace

The playback bar measures its fixed controls and allocates remaining cells between the title and seek rail, shortening the rail on constrained terminals. Existing keyboard seeking remains available. A present playback subject without title metadata uses the localized track-id fallback; missing metadata alone never renders the no-active-track state.

Visible workspace status shortcuts also activate their displayed action on click.

Mouse targets are valid only for painted cells. Editors invalidate their targets after an event until the next render; runtime-backed row targets carry identity as well as position so a refreshed list cannot activate a different row through old geometry. The mouse preference gates editor input as well as the workspace.

Track navigation moves focus by row/page/endpoints; group navigation focuses the first track in the previous/next section.
At the first or last section boundary, an unavailable group step leaves focus, marks, and an active visual range unchanged; it does not reselect the current section's first track.
Mouse wheel over the track table moves focus by the configured wheel step (three tracks by default).
Dragging the table scrollbar maps its visual position to a focused track.
Clicking a section header focuses its first track; dragging a header edge previews a width in terminal cells and releasing it commits canonical state for the current base list.

After runtime workspace restore, `LibraryController` reads the active process-local `ViewId` and materializes that view directly.
It does not navigate by `ListId`, so multiple filtered views over one list remain distinct and construction adds no history entry.
Only a missing or unusable active view opens the All Tracks fallback.
A fallback or later plain-list navigation supplies that list's saved or recommended presentation as `NewViewDefault`; runtime view reuse still retains the exact active presentation.
A successful user presentation choice records the current base list's preference, while filtering and exact workspace attachment do not.
Reload follows the same direct materialization path; if the controller's view disappeared, it attaches a different active workspace view when available and otherwise opens All Tracks.

Views and `:view` (including aliases) share the [List eligibility rule](../presentation/list-preference.md#resolution). On All Tracks, confirming the disabled Manual Order row keeps Views open; the command gives the same reason. Neither rejection changes the presentation, history, or saved preference. Custom flat unsorted presentations are not the built-in Manual Order preset and remain eligible.

#### Fit Lists beside Tracks

Lists starts pinned by default with keyboard focus on Tracks. Pinning is a layout preference, not a focus or popup state. Each frame lays out Detail, if visible, and docks Lists only if the remaining space meets these budgets:

| Part of the workspace | Horizontal budget in terminal cells                                                                                                                                                                               |
| --------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Lists                 | 26 framed columns at automatic width; with an explicit request, the pane can shrink to 18.                                                                                                                        |
| Tracks                | Reserve 72 track-content columns, plus its two borders and one inner padding column in the docking calculation.                                                                                                   |
| Detail                | Subtract its effective framed width, allowing for a shared border. An explicit resize has a 24-column framed minimum where space permits.                                                                         |
| Dividers              | `single` shares a vertical border and joins its top and bottom edges. `double` keeps adjacent borders and separate corners; each double divider costs one extra column when docking and allocating track columns. |

A narrower terminal can constrain the displayed widths further or undock Lists; clamping never overwrites preferred widths. Widening or reopening a pane restores its request when space permits.
Settings publishes the global `panelSeparator` only after a successful save. The change does not toggle pinning or move focus unless docking availability changes.
If a focused Lists pane loses its space, focus returns to Tracks. If a pinned pane regains space while its temporary chooser is open, the chooser closes and the pane takes focus; an unpinned chooser stays open.

The permanent Lists and Detail frames omit headings; popups keep their titles. Inside each frame one cell per side separates the content from the border. The track highlight does not cover that padding, and the Lists and Tracks scrollbars use the right cell.
Below the tree, only the selected List's local expression appears, dim and aligned with the content gutter. An empty expression costs no row. Otherwise the caption wraps at word boundaries where possible and uses at most two rows, ellipsizing overflow without splitting wide glyphs.
There is no caption separator or local focus hint: the Lists status shows the executable return-focus shortcut, or fixed Tab during search.

#### Access and navigate Lists

`l` / `:lists` focuses a visible pinned pane or toggles the popup when no pinned pane is visible. It never changes the layout preference. `L` / `:sidebar` independently toggles pinning through `tui.workspace.togglePinnedLists`; enabling it focuses Lists when there is room to dock, and disabling it returns to Tracks. Pinning from the chooser closes the popup and opens the tree when space permits. At insufficient width the pin preference is retained and temporary popup access remains available. Printable search and editor input retains priority over these root bindings.

The divider contains a centered `‹` control. Hovering either column of a double divider, or the single shared column, highlights the separator and its arrow; only the one-cell arrow is a collapse target. Hover regions are cleared with each frame and suspended behind stronger surfaces. When unpinned and enough width remains, the track frame's left edge contains `›`; both controls use the same pin toggle as `L`. Neither is interactive underneath stronger surfaces. Dismissing a popup does not change pinning. The track footer contains a List/current-list button, presentation selection, and counts. Only the List button is suppressed while the pane is actually docked; its mouse box is cleared every frame. Otherwise it anchors the separate chooser above itself. Frame picker anchors are read during layout, after the background frame publishes its button rectangle, so the first frame and subsequent resizes use the current button position. The status Lists shortcut follows the same visibility rule.

The configurable `tui.workspace.switchFocus` action defaults to Tab and Shift+Tab: Lists returns to Tracks; Tracks enters Lists only when docked. Escape returns to Tracks without unpinning. Focus changes preserve track selection and unfinished visual ranges. A stronger editor, popup, or shell input suspends Lists focus without a second return-focus value. Successful Enter, current-track reveal, and history navigation return to Tracks.

All Tracks and user Lists follow shared preorder and the existing zero indentation for children of the virtual All Tracks root. User descendants add bounded indentation, with disclosure triangles separate from the active-List marker and keyboard cursor. Flat lists omit the unused disclosure column. The selected List's expression occupies a bounded detail line. Parent Lists remain navigable Lists, not folders.

Up/Down or j/k, page keys, and Home/End move the List cursor. Left/Right collapse/expand before root seek bindings; bracket seek and other explicit playback controls remain available. Track selection, edit, reload, and filter shortcuts cannot leak through Lists focus; explicit palette commands retain their existing actions. The pinned pane omits its search row while idle. Lists focus exposes a clickable `/ Search` status action that enters the same local search protocol as `/`, independently of the workspace filter binding. Its hit region is cleared when searching, unfocused, or under stronger surfaces. Escape clears search and removes the row. `/` searches names and ancestor paths using Unicode case-insensitive matching, temporarily revealing matches with non-activatable ancestor context. While searching, printable keys and caret keys edit text; Up/Down and pages move matches. Escape clears search first; Tab/Shift+Tab always clears search and returns to Tracks even when the configurable focus action has another binding. Empty matches have no activation target. Suspending search under a stronger surface retains it; permanent focus departure clears it.

Enter opens by List ID and returns to Tracks. A row click opens once and keeps docked Lists focused, while the separate chooser closes after successful activation. Activating the already active List preserves the exact view, filter, presentation, selection, marks, and history. A disclosure click only changes expansion; wheel and scrollbar movement change the List cursor without navigation, and wheel does not steal keyboard focus. An outside chooser left press dismisses the popup and is consumed; an outside docked-pane press can operate Tracks immediately. Foreground popup/editor/input ownership precedes either pane, and private search hit regions are invalidated before conditional rendering.

Tree refresh reconciles cursor and expansion by ID. A deleted cursor falls back through surviving ancestors, active List, All Tracks, then the first row. External navigation reveals the active path while preserving an independently browsed cursor. Pre-navigation failure leaves the active view intact; attachment failure after runtime navigation explicitly reconciles the runtime active view or opens the existing All Tracks fallback, and reports failure rather than claiming success with stale content.

The chooser restores the compact anchored list presentation: All Tracks plus every saved List in shared preorder, with indentation, `[L]` labels, and expressions. `LibraryNavigation` builds this flat projection independently of the tree's expansion and search. Its selected row is independent of the tree cursor, refresh retains a surviving selected ID, and opening the chooser reveals the active List. The chooser supports local `/` search, selection keys, Enter, and stable-ID mouse activation. Popover hints show local actions and Escape; they omit the redundant opening/toggle shortcut while retaining that binding. The Lists chooser also shows its effective pin shortcut. Its height follows the row count up to a bounded viewport; its width follows content within the terminal. Search owns printable `l` and `L`; otherwise the effective access and pin bindings close the chooser or switch to the pinned tree.

The Lists pin preference is persisted in the per-library layout file's versioned `navigation` group. Cursor, expansion, search, focus and chooser visibility are session-local. Panel width requests persist independently in the `panels.navigation` and `panels.detail` fields, with zero selecting automatic width. The existing `navigation.enabled` field stores this preference. Missing or malformed navigation state defaults to pinned independently of other groups. The existing coalesced layout writer saves all groups together; failure leaves live state and dirty state intact, posts one localized warning, and retries at the next requested checkpoint or normal exit.

#### Navigate with Go to

Go to is a modal navigation menu opened by the effective `tui.navigation.openGoTo` shortcut (default `g`) or `:goto`. It owns fixed suffixes `t`/`a`/`b` for the playing track/artist/album and `[`/`]` for backward/forward view history. Arrows, j/k, page keys, Home/End, and Enter provide menu selection. The menu has no timeout; unrelated keys dismiss it without reaching root actions. Escape closes the menu before cancelling any visual selection. Text input and stronger editors retain their own input ownership.

While Go to is open, its clickable status hints replace ordinary shortcuts and activity. The footer shortens labels before keys and never clips a control:

| Footer width       | Controls that fit                                                                                    |
| ------------------ | ---------------------------------------------------------------------------------------------------- |
| Fewer than 3 cells | None; there are no mouse targets.                                                                    |
| 3–22 cells         | Reserve the complete `Esc` first, then admit whole suffix chips with their separators in menu order. |
| 23 cells or more   | All five suffixes and `Esc`.                                                                         |

Disabled destinations remain dimmed where their chips fit. Omission from the footer does not remove an action from the menu or keyboard.
Destination clicks in the menu and footer compare the painted now-playing information and Back/Forward availability with current state, so a changed playback subject cannot redirect an old click. Successful navigation or cancellation restores ordinary hints.

#### Inspect the focused track

The detail sidebar follows the focused track while leaving keyboard navigation with the workspace. The effective `tui.detail.focus` action (default uppercase `D`) opens it if needed and explicitly transfers keyboard focus to its sections. Ordinary `Tab` / `Shift+Tab` continue to alternate Lists and Tracks; from Detail either first returns to Tracks. Detail owns `j/k` and Up/Down to select its Metadata or Audio Properties section, Enter to toggle, Left/Right to collapse/expand, and PageUp/PageDown to scroll. The selected header is revealed and highlighted only while Detail has keyboard focus. Escape returns to Tracks without hiding Detail or cancelling a visual selection. Playback shortcuts remain available. A modal or text input takes precedence over Detail keys.

When title metadata is absent and a source filename is available, expanded Metadata shows a localized File Name row; it never substitutes that filename into title metadata.
The collapsed header retains the existing filename identity fallback.
Metadata, including Tags, starts expanded and Audio Properties starts collapsed. Collapsed headers retain a title/artist or audio summary. Section expansion survives changing tracks and hiding the sidebar within the session. Clicking a visible header toggles that section without transferring keyboard focus; header hits are clipped to the text viewport and validated against the rendered track. Clicking the track table returns keyboard focus to Tracks. Hiding a focused Detail also returns to Tracks. The status row displays the local section controls while Detail is focused.
While it is visible, track and group navigation, wheel selection, scrollbar drag, section-header selection, column resizing, playback, seek, volume, Quick Filter, and the Command Palette all remain available against the reduced workspace geometry, and the pane follows the selection the workspace produces.
Entering text input suspends those gestures for the duration of the input without closing the inspector.
A track-row click focuses that track; two consecutive unmodified clicks on the same track within 400 ms start playback. Other keys, wheel movement, and intervening controls break the click pair.

#### Browse and dismiss popovers

Help centers a panel up to 88 columns wide over the full-width workspace. Its two-column body pairs effective shortcuts with descriptions, grouped by navigation, browsing, list order, playback, selection and editing, panels, and application. When an action has no shortcut, its row falls back to a command if defined; otherwise its shortcut cell is blank. The Command Palette provides the full inventory.
Only the body scrolls. Opening and closing Help preserves the filter, focused track, and track-table layout.

In the Lists chooser, Presentations, and Output, Up/Down or j/k, page keys, Home/End, and the wheel move selection outside search. Return or a row click activates an eligible selected or clicked row. In Help, Quality, and Notifications, the same keys and the wheel scroll content. Page keys use the rendered viewport when available.
The wheel scrolls Detail too, but keyboard navigation stays with the workspace unless Detail has explicit focus and uses its section controls.

Quality, Output, Presentations, and Notifications each use their effective toggle shortcut to open or close. Help also closes on its effective Help shortcuts. Popovers name the overlay in the frame title; those with extra context, such as the active view or output device, show it on the right. Picker rows lead with the name and dim any description.

Centered and anchored popovers leave a clear one-cell halo outside their hit regions. Foreground controls consume clicks before the workspace. A left press outside Lists, Quality, Output, Presentations, Notifications, or Help dismisses the popover and consumes the press, even on another trigger. It takes a subsequent click to operate the background control. Closing retires stale quality hover.
These popovers use Escape or outside dismissal rather than a `×` button. Detail and Settings do not dismiss on outside clicks. Settings has an explicit close button for unsaved changes; [TUI track authoring](tui-track-authoring.md) owns editor dismissal and unsaved changes.

#### Find settings and actions

Settings has one right-aligned, clickable footer. It wraps between actions on narrow terminals. Language and renderer explanations stay beside their settings; internal action ids are searchable but do not occupy the footer.
On the Keyboard page, `/` or the footer opens search. Escape is labeled Clear there, not Close; empty results offer no edit action. Chord editing and failed-save recovery replace ordinary actions with the applicable save, cancel, retry, and discard controls.

Presentations offer `/` local search by Unicode case-insensitive substring. Settings Keyboard searches localized action labels and stable action ids. Search keeps owner row indices and identities, restricts navigation and activation to matches, and never changes the workspace filter. While searching, printable keys including j/k edit the query; Up/Down and page keys move through matches. Return activates the match, and an empty result cannot activate a hidden row. Escape first clears local search; the next Escape closes the overlay. Switching panels or Settings pages clears that panel's query.
Outside local search, Escape closes the active overlay.
Each overlay presents its interaction hints in its own panel; the status row retains independent activity feedback without repeating the panel title or hints.
Go to keeps its dedicated clickable navigation status row in addition to the menu's selection-key footer.
Opening an overlay does not post a transient notification; closing it retains the existing status feedback and errors remain visible.
Notification `x` locally suppresses the compact activity entry according to the shared activity model.

#### Layer foreground surfaces

Overlays are composed over the root in one place, ordered by keyboard ownership: whichever surface answers for every key is drawn last, so no candidate below it can cover a surface that still owns input.
Each candidate is built only once those above it decline, so an overlay that is not drawn publishes no hit regions for rows nobody can click.
Kitty artwork is suppressed when an actual foreground rectangle intersects its cover rectangle or clearing halo, including the Lists chooser and shell input; it returns when the obstruction closes. Full-surface editors and exit retain unconditional suppression.
Only a centred modal surface dims what it covers, and only when backdrop dimming is enabled. It stands on its own rather than beside the control that opened it; the workspace under it is not what the user is acting on.
An anchored panel leaves the workspace lit, and Quick Filter must, because the track table behind it is the very thing the typing changes.

### Playback dock and seek

The single-row dock contains the Soul transport and quality control, playing-track metadata, mode button, output badge, elapsed time, responsive seek rail, duration, and volume.

**Navigate without changing playback.**

| Painted field       | Destination                                                                                                                                     | When metadata is missing                        |
| ------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------- |
| Title (bold accent) | Reveal the current track.                                                                                                                       | A valid track id supplies a clickable fallback. |
| Artist (normal)     | Open All Tracks filtered by the exact artist. The full value is one quoted constant, even with embedded quotes, backslashes, or multiple names. | No artist link.                                 |
| Album (dim)         | Open the All Tracks `albums` presentation and focus the current track's group.                                                                  | No album link.                                  |

Artist and album navigation checkpoint the live filter and presentation; Back/Forward restores the previous view without rewriting saved List predicates. The `artist` and `album` commands reach the same destinations when the text does not fit. An invalid playback track has no metadata links.

**Find a link on a narrow dock.** The dock separates linked fields with `·`, or `/` between artist and album. It omits trailing fields if needed and shortens the visible fields at display-cluster boundaries. Title gets priority, then artist, while shorter linked fragments remain readable.
Only painted text has a hit region. Hover highlights the pointed field without changing its size; separators, padding, hidden fields, and metadata behind input or overlays are not targets. Editors, text input, and floating-menu dismissal keep pointer ownership. A click on stale playback identity or metadata is consumed without navigating until the new state is painted.

**Adjust volume.** Click the volume label to toggle mute, or use the wheel over it to adjust gain by the configured step. The localized mute label replaces the percentage without changing the stored level.

The Soul control toggles playback on click; hover shows quality detail without opening a modal overlay.
Soul and the effective `playback.playPause` shortcut (Space by default) pause active playback, but cannot resume paused playback while a requested output is pending. From Idle they resume a restored track that succession still owns. Otherwise, with a ready output, Idle or Error can request a new start from the track selection.
Stop is an idempotent silent no-op when playback is already Idle.

Seek press begins a shared `SeekInteraction` gesture and captures the current playback occurrence and duration. Pointer motion publishes guarded queued preview seeks, and release publishes the matching guarded queued final seek through `PlaybackPositionViewModel`.
Release beyond the rail clamps to the captured rail range. If playback is replaced while the pointer remains held, continued motion and release stay inert; only a fresh press may target the replacement.
Keyboard seek captures the snapshot occurrence and asks the same view model for the configured relative step, which defaults to five seconds.
The queued runtime command samples live elapsed at execution and clamps the target to the current track; it is inert without the matching occurrence and a positive duration. A final seek to the end while playing follows ordinary track completion; a paused track can hold that position.
Keyboard volume asks `VolumeViewModel` for a clamped relative change at the configured step (five percentage points by default), including the shared rule that an increase in the clamped level clears explicit mute.

### Rendering

Panels use titled-frame chrome with one-cell horizontal body padding. Bodies with a built-in scrollbar use the shared panel-body layout so the scrollbar occupies the right padding cell; the surrounding frame adds no further body padding. The track width budget includes that scrollbar column. Detail scrolls its text without drawing a scrollbar.
Detail meets Tracks at the configured single or double divider, with one cell of content padding on each side. Its middle `›` control hides it and the collapsed right edge offers `‹` to show it again; both sidebar arrow controls highlight their single-cell click target on hover. Moving away or toggling clears that feedback; the effective Detail toggle (default `d`) changes the same state. It has no close cross, open/close notification, or modal footer. Escape leaves the sidebar visible. Its outer frame is its only chrome: present cover art renders as artwork alone, with no nested title, separator, or border, and block and Kitty delivery reserve the same terminal cells.
Automatic Detail width follows the active locale and cover width, not the selected track. Explicit sidebar widths override automatic sizing when space permits.
The `revealIndicatorsOnHover` appearance preference defaults to false. When enabled, both sidebars render their idle arrow cells as vertical border strokes without removing or moving the click targets. Hovering a divider or the full collapsed side border reveals its centered arrow; moving away hides it again. Single and double separators follow the same rule. Clicking elsewhere along the border does not toggle the pane.

Pinned Lists and Detail dividers support left-button drag outside the centered indicator cell. An admitted drag owns motion and release across pane boundaries, highlights the divider, and previews terminal-cell widths without persisting them. Left-button release commits; Escape, another key, a modal surface, or terminal geometry changes cancel. Escape is consumed by the cancellation; another ordinary key first rolls back the pointer preview, then performs its normal action against committed state. The drag's own horizontal reflow does not cancel it. Shift+Left/Right moves the focused sidebar's divider one cell in screen direction and checkpoints immediately; these fixed contextual keys do not change playback bindings. These quick steps apply only to a focused sidebar, outside text entry and popovers.

`tui.workspace.beginPanelResize` (default Ctrl+W) enters a keyboard layout adjustment from any workspace focus. It selects the focused sidebar's divider, or the left visible divider from Tracks, falling back to the right. Tab/Shift+Tab cycles only visible dividers. ArrowLeft/Right, h/l, and Shift+Left/Right preview one-cell screen-direction moves. Enter or the entry shortcut applies both sidebar preferences in one checkpoint; Escape, a consumed left mouse press, an input/modal surface, or terminal geometry change discards the whole preview. Other keys and mouse events are consumed while adjusting, except the existing Ctrl+C exit protocol. Workspace focus and track selection remain unchanged. The highlighted divider and target-specific status hint expose the mode. No visible divider means no mode. Text entry keeps its Ctrl+W editing command.

`PanelResize` owns the shared screen-delta and width-limit calculation for pointer drags, immediate keyboard steps, and keyboard-mode previews. `EventController` owns admission, preview lifetime, target switching, and commit/cancel; rendering consumes the same effective widths and selected target.

Divider movement respects the [workspace width budgets](#fit-lists-beside-tracks): a resize cannot take space from the minimum track body.

#### Read metadata and artwork in Detail

Metadata, audio properties, and tags share two columns: muted labels, then left-aligned values separated by whitespace, not colons. The title value is emphasized. Title, artist, album, year, track number (with disc and total when applicable), and duration have labeled rows. Optional fields include composer, conductor, ensemble, soloist, work, movement, and genre.
Empty values and an album artist identical to the artist are omitted. Collapsed sections retain a track identity or compact audio summary. Tags in Detail are read-only; there is no inline editor or permanent Edit button.

| Part of Detail       | Budget                                                                                                                                                                                                 |
| -------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Automatic pane width | Allow for the localized labels plus 24 value cells, the cover width, and the localized no-selection text. Cap the framed width at half the terminal.                                                   |
| Labels               | Measure the widest localized label, cap the text at 12 cells, and add a 2-cell gap. The column including the gap takes at most 40% of the body.                                                        |
| Values               | Leave at least 12 cells at the `80x24` target. Wrap at whitespace where possible and align continuations with the value column. If a display cluster cannot fit, use an ellipsis rather than split it. |
| Artwork              | Require 25 available pane rows: 12 for artwork, 3 for chrome and separation, and at least 10 for metadata. This threshold does not depend on the selected track.                                       |

Artwork stays fixed above the independently scrolling text. Scrolling stops at the last page; selecting a different track resets the scroll to the top.

#### Read the table and its selection

When there is room for its footer, the lower workspace frame identifies the view on the left, alongside the List button if Lists is not docked. Selection and counts go on the right. Dim vertical rules separate track columns, with a one-cell gutter on each side; a dim horizontal rule completes each section header. Values that do not fit end in an ellipsis. A missing field renders one aligned, dim `-`; a literal `-` keeps full emphasis.

Rows selected in the focused pane and controls using the shared hover style paint bold black text on yellow after their content, overriding accents such as the now-playing caret or active List. The selected track instead keeps an accent when Tracks loses focus. Marks reverse the resolved colors without reserving a palette slot or a table cell: a marked row outside the focused track selection reverses its own colors; the focused marked track reverses the interactive pair.

#### Render the playback Soul

The playback Soul animation consumes shared UIModel aura/color/timing policy while terminal code chooses braille geometry.
Its partial arc travels clockwise like the GTK and brand Soul, with its ends positioned by distance along the braille ring.
Breathing lengthens the arc by the golden ratio, mirroring GTK's stroke expansion; the cyan core rides the arc head, the quality aura fills its tail, and a cell holding only a partly covered edge dot dims instead of popping.
Transport errors draw a ring broken at its top in the dormant aura rather than a clipped-signal color.
Opening, Buffering, Playing, and Seeking keep periodic animation refresh active, and it continues while a shared pause coast or aura cross-fade settles; reduced motion settles them at once. Elapsed-time interpolation advances only in the transport states identified as playing by `PlaybackPositionViewModel`.
Short terminals keep the dock to one row before reducing track-table height further.

## Failure and cancellation

Unavailable playback actions post warning notifications rather than inventing terminal-only error state; the idempotent Idle Stop exception remains silent.
An ineligible or unknown presentation choice posts transient Info. If the displayed view is no longer the workspace's active view, both Views and the command warn instead of changing another view. Reopening the intended List reattaches it even if the terminal still shows its rows. An unexpected failure to apply a presentation logs the diagnostic and posts a localized Warning in history.
Stale section/output rows are rejected and reported.
Submitting a filter calls the typed runtime view boundary before replacing terminal rows.
On Error, `LibraryController` preserves the draft, active source/view, rows, sections, and selection; `EventController` logs the failure and posts an Error notification instead of reloading.
An invalid live expression remains visible in the Quick Filter panel without posting one notification per debounce tick.
Return or Escape performs one immediate final application before the panel closes; a recoverable expression error posts one Warning, while a command-level failure posts one Error.
Replacing, closing, or destroying Quick Filter input requests stop on the pending timer; a stopped or obsolete generation cannot mutate shell or library state.
Text-input or overlay entry first resynchronizes seek identity, then cancels a still-current active seek preview by committing the current runtime elapsed value as the final stabilization point and resetting the gesture.
A replaced occurrence is reset without dispatching stale stabilization.
An interrupted column drag instead discards its preview. Overlay changes, text input, list changes, unrelated pointer presses, and teardown therefore produce no column-layout model change or save.

`:scan` and `:rescan` start one eager `LibraryScanController` flight through `uimodel::runLibraryScanAsync`.
`:scan cancel` requests stop on that flight.
Start, cancel, retire, and completion bookkeeping run on the TUI callback executor, which is the FTXUI dispatch lane; `phase` is unsynchronized.
A start while Running posts a transient already-running notice; a start while Cancelling posts a transient cancellation-in-progress notice.
`async::OperationCancelled` is silent; a returned `LibraryScanOutcome`, including Failed, is presented even if cancellation is in progress.
Retirement suppresses late presentation.
Scan progress continues to use `ActivityStatusViewModel` observing `LibraryJobs`.

## Selection and marks

**Move focus or mark tracks.** With Tracks focused, Up/Down and the default `k`/`j` move focus without changing marks unless a visual range is active. By default, `m` toggles the focused track's mark, `Shift+A` marks every materialized track in the view, and `u` clears all marks. Marked rows remain distinguishable in every focus state.
A plain row click clears marks and focuses the clicked track; Ctrl+click toggles its mark. Playback Enter, Detail, and cover art follow focus, not marks. Marks affect the status count and the effective selection that `publishSelection()` publishes from `selectedTrackIds()`.

**Grow or finish a range.** The visual-selection action (default `v`) anchors a range at the focused track. Each move combines the starting marks with the inclusive range from anchor to focus in flattened track order. Shift+click starts or extends the same range. The footer names visual mode before the count so the user can tell whether the next motion will change the selection.
Press the visual-selection shortcut again to confirm. Toggling a mark commits the range before toggling the focused track; marking all or resetting marks also ends the range.

**Cancel without losing earlier marks.** With Tracks focused, Escape restores the starting marks that still survive in the view and leaves an open Detail visible. A modal overlay takes Escape first. If Detail has focus, the first Escape returns to Tracks without cancelling the range.

**Reconcile changes to the view.** A successful effective filter change or navigation to a different view clears marks; a successful no-op filter preserves them. Attaching a different view starts at the top. Reattaching the same view reconciles focus and marks instead; confirming the current List in the chooser leaves the current view, focus, and marks unchanged.
On same-view reload or presentation change, marks are intersected with surviving track ids. An active range is derived again in the new row order from its reconciled starting set, anchor, and focus. If the anchor has disappeared, the range ends and keeps the surviving marks it had reached; losing the anchor does not restore the starting set.

## Track authoring entry

The effective edit shortcut (shipped as `e`), `:edit`, and `:properties` open the full Properties editor over `selectedTrackIds()`.
The quick tag entry (`t` / `:tags`) opens its tags-only surface over that same effective selection.
The [TUI track-authoring specification](tui-track-authoring.md) owns target capture, coherent preparation, editor pages and validation, recovery, submission, and admitted-write lifetime.
The `:list new`, `:list edit`, and `:list delete` commands open the Saved-List authoring surfaces; the [TUI List-authoring specification](tui-list-authoring.md) owns their target rule and workflow, and the same entry transition below applies.

Opening either editor retires command and filter input, cancels the filter debounce and pointer gestures, rolls back a column preview, and commits a running visual range into the mark set; the Detail inspector's visibility preference is preserved rather than closed.
The range's rows stay marked because they formed the effective selection at entry, but the anchor is retired so later motion cannot reshape that completed selection and a library change under the modal cannot re-derive the range against rows the edit itself reordered.
The editor is composed over the live workspace with exclusive input ownership; the workspace remains visible but inert, and cover art is neither requested nor painted until authoring closes.

## Exit and teardown

**Request exit.** The effective quit shortcut (default `Shift+Q`), `:quit`, terminal Ctrl-C, and handled platform signals all request the same App-owned `ExitController`. The POSIX watcher requests exit on SIGINT, SIGTERM, or SIGHUP. The Windows watcher requests exit on console Ctrl-C, Ctrl-Break, close, logoff, or shutdown events it receives; Windows can still end the process if cleanup outlasts its grace period.

**Wait for admitted writes.** The gate checks both authoring controllers for a pending submitted write, then immediately retires scan and editor presentation and transient input. A submitted List save or deletion uses the same [write-settlement handshake as Properties](tui-track-authoring.md#admitted-write-lifetime-and-exit): loop exit waits for settlement or a second exit request. Ordinary input is consumed during the wait; Ctrl+C can still request exit. Without a pending write, the gate posts loop exit after retirement. Input dispatch does not stop playback early.

**Retire system media.** For an accepted MPRIS root Quit, the bridge submits the empty D-Bus reply before the deferred exit callback can run on the owner executor. The first exit request closes command and artwork admission immediately, even while waiting for a metadata write. System media has no separate exit or write-wait policy.
The private bus session starts local close when its asynchronous flush finishes or reaches its 250-millisecond deadline. Closing the connection releases its bus name. The session joins without further progress on the TUI executor. The deadline neither bounds the rest of teardown nor guarantees that the peer receives the last reply.

**Checkpoint and stop.** Before capturing session state, normal teardown cancels Quick Filter debounce, seek, scrollbar and resize gestures, cover work, and scan presentation. Cancelling a still-current seek commits the runtime elapsed position, not the uncommitted preview; a replaced occurrence receives no stale seek.
Teardown checkpoints workspace, then playback, then requests playback stop. Frontend observers and controllers are destroyed before runtime shutdown. `ScreenInteractive` outlives the runtime executor that borrows it; neither frame timers nor executor callbacks can access the screen beyond the owning application's lifetime.

## Persistence and versioning

TUI workspace config defaults to `<root>/.aobus/tui-workspace.yaml` and follows the workspace session contract.
The runtime uses one `ConfigStore` writer for both its `workspace` and `playback-session` groups in that file.
The independent `<root>/.aobus/tui_layout.yaml` file has one TUI writer for `trackView.columnLayouts`, `trackView.presentations`, `navigation`, and `panels`; committed layout changes save these groups atomically.
Column widths in that document are terminal cells and are never converted to or from desktop pixels.
Workspace and presentation state are restored before terminal view attachment; playback observation and restore follow before the event loop.
Normal exit discards any unfinished column or sidebar-width preview, retries a pending failed presentation checkpoint, checkpoints both runtime groups, then stops playback before runtime shutdown.

Restored workspace state includes open view configurations, exact active-view choice, and custom presentation presets; it excludes track selection and navigation history beyond the reconstructed initial point.
Restored playback state includes source/filter/order, current track and position, modes, volume, and mute without autoplay.
Restored TUI presentation state includes each list's column order, visibility, fixed cell widths or flexible weights, and preferred presentation id.
Active overlay, input draft/mode, the original Quick Filter editing draft, hover, and pointer gestures are session-local and unversioned.
The preferred output route is stored separately in the global TUI application-preference file.
The same global `<config>/tui.yaml` document supplies the `shortcuts` group over terminal defaults.
TUI loads that group before constructing dispatch and render owners. Settings persists an accepted candidate before replacing the stable-address keymap and dispatch/hint plan values. Preference and output writes share the same store and preserve sibling groups.
Exact startup paths/options and managed locations belong to the TUI and persistence references.

## Frontend observations

The detail pane remains beside the track workspace and shows a terminal cover-art representation plus selected-track fields.
Fields appear when they carry a value.
When title metadata is missing, the track path supplies a separately labeled File Name row if a filename is available.
Kitty, block, automatic, and disabled cover modes come from the saved preference, with an explicit command-line option taking precedence for the session. Without that override, Settings changes the effective renderer live, canceling the previous cover request and hiding stale Kitty placement. With an override, Settings still saves the preference for subsequent launches while the session renderer stays unchanged.
On a cover change, the pane renders one compact unavailable line until asynchronous delivery completes; an older selection cannot replace the current cover.
A frame that reserves no artwork cells leaves an invalid cover box behind, which is how out-of-band Kitty paint state learns to delete a stale image.
The notification center can be opened explicitly even when compact status is not the only visible affordance.

## Implementation map

- [`TerminalTitle.cpp`](../../../app/tui/TerminalTitle.cpp) owns the cached playing-track title and balanced terminal title stack; the runtime `LibrarySnapshot::formatTrack` boundary evaluates one track. [`SoulButton.cpp`](../../../app/tui/SoulButton.cpp) supplies the shared glyph frames for the dock and title.
- [`App.cpp`](../../../app/tui/App.cpp) composes runtime, screen, render, controllers, lifetime, and optional Linux system media; [`MprisBridge.cpp`](../../../app/systemmedia/linux/MprisBridge.cpp) and [`MprisBusSession.cpp`](../../../app/systemmedia/linux/MprisBusSession.cpp) own the shared adapter and private bus session.
- [`CoverArtLoader.cpp`](../../../app/tui/CoverArtLoader.cpp) owns asynchronous selected-resource delivery and stale-result suppression; [`CoverArt.cpp`](../../../app/tui/CoverArt.cpp) owns bounded decode and terminal transforms.
- [`ShellInteractionModel.cpp`](../../../app/tui/ShellInteractionModel.cpp) owns text-input and overlay state.
- [`GoToMenu.cpp`](../../../app/tui/GoToMenu.cpp) owns navigation menu availability, menu/footer rendering, and reflected action targets. [`GoToMenu.h`](../../../app/tui/GoToMenu.h) defines the shared playback/history snapshot; [`StatusBar.cpp`](../../../app/tui/StatusBar.cpp) selects hints for the active interaction state.
- [`Command.cpp`](../../../app/tui/Command.cpp) owns command parsing, discovery vocabulary, and command-to-key-action mappings.
- [`Keymap.cpp`](../../../app/tui/Keymap.cpp) owns stable terminal action descriptors, TUI-local defaults, the FTXUI projection whitelist, collision selection, and the immutable dispatch/hint plan.
- [`ListSearch.cpp`](../../../app/tui/ListSearch.cpp) owns local query editing and matching over owner row indices; [`SelectionNavigation.cpp`](../../../app/tui/SelectionNavigation.cpp) translates the fixed list-navigation protocol.
- [`CommandCompletion.cpp`](../../../app/tui/CommandCompletion.cpp) owns command and presentation completion plus explicit filter-argument routing; [`App.cpp`](../../../app/tui/App.cpp) composes the Command Palette and live Quick Filter callbacks.
- [`EventController.cpp`](../../../app/tui/EventController.cpp) owns keyboard/mouse dispatch and transient-interaction cancellation, and forwards graceful exit without owning `ScreenInteractive`.
- [`LibraryScanController.cpp`](../../../app/tui/LibraryScanController.cpp) owns the single restartable scan task.
- [`ListAuthoringController.cpp`](../../../app/tui/ListAuthoringController.cpp) and [`SmartListEditor.cpp`](../../../app/tui/SmartListEditor.cpp) own the Saved-List authoring flow; see the [TUI List-authoring specification](tui-list-authoring.md).
- [`ExitController.cpp`](../../../app/tui/ExitController.cpp) owns the idempotent graceful-exit gate; [`SignalExitWatcherPosix.cpp`](../../../app/tui/SignalExitWatcherPosix.cpp) and [`SignalExitWatcherWindows.cpp`](../../../app/tui/SignalExitWatcherWindows.cpp) post those requests from platform signals.
- [`LibraryController.cpp`](../../../app/tui/LibraryController.cpp) owns exact runtime-view attachment, row materialization, preference-aware plain-list navigation, and reload fallback.
- [`LibraryNavigation.cpp`](../../../app/tui/LibraryNavigation.cpp) projects the shared List preorder into chooser entries and labels; [`LibraryChooser.cpp`](../../../app/tui/LibraryChooser.cpp) paints the transient chooser independently of pinned-tree search and expansion.
- [`PanelResize.cpp`](../../../app/tui/PanelResize.cpp) owns constrained width previews over [`PanelWidths.h`](../../../app/tui/PanelWidths.h); [`PanelResizeEvents.cpp`](../../../app/tui/PanelResizeEvents.cpp) owns pointer and keyboard resize lifecycles, cancellation, and commit.
- [`ListNavigationModel.cpp`](../../../app/tui/ListNavigationModel.cpp), [`NavigationPanel.cpp`](../../../app/tui/NavigationPanel.cpp), and [`NavigationEvents.cpp`](../../../app/tui/NavigationEvents.cpp) own List cursor/search projection, terminal geometry, and scoped interaction.
- [`Render.cpp`](../../../app/tui/Render.cpp) and [`Style.cpp`](../../../app/tui/Style.cpp) own common terminal composition and styling; [`CommandPalettePanel.cpp`](../../../app/tui/CommandPalettePanel.cpp) owns command/filter completion panels, and [`StatusBar.cpp`](../../../app/tui/StatusBar.cpp) owns the Quick Filter input row.
- [`TrackDetailLines.cpp`](../../../app/tui/TrackDetailLines.cpp) projects populated track fields and the separately labeled filename fallback into detail rows.
- [`TerminalTrackColumnLayout.cpp`](../../../app/tui/TerminalTrackColumnLayout.cpp) projects shared column state into terminal cells; [`TrackTable.cpp`](../../../app/tui/TrackTable.cpp) owns track-table output; [`LayoutStateStore.cpp`](../../../app/tui/LayoutStateStore.cpp) owns the presentation file.
- [`PlaybackPanel.cpp`](../../../app/tui/PlaybackPanel.cpp) and [`SoulButton.cpp`](../../../app/tui/SoulButton.cpp) own the dock.

## Test map

- [`TerminalTitleTest.cpp`](../../../test/tui/TerminalTitleTest.cpp) protects expression compilation, metadata invalidation, title ownership, Soul composition, and control-safe output. [`SoulButtonTest.cpp`](../../../test/tui/SoulButtonTest.cpp) protects shared playback-state glyphs.
- [`ShellInteractionModelTest.cpp`](../../../test/tui/ShellInteractionModelTest.cpp) protects input modes, touched state, mode-local history, completion-range application, and overlay state.
- [`TextFieldModelTest.cpp`](../../../test/tui/TextFieldModelTest.cpp) protects grapheme-aware caret and Unicode word editing. [`ListSearchTest.cpp`](../../../test/tui/ListSearchTest.cpp) protects local query ownership and filtered selection.
- [`CommandTest.cpp`](../../../test/tui/CommandTest.cpp) protects command parsing and key-action mappings.
- [`KeymapTest.cpp`](../../../test/tui/KeymapTest.cpp) protects shared action identities, independent terminal defaults, terminal aliases and omissions, collision order, unbinding, and coupled dispatch/hint selection.
- [`EventControllerTest.cpp`](../../../test/tui/EventControllerTest.cpp) protects input routing, live-filter debounce/cancellation, completion acceptance, key/mouse modality, seek, teardown stabilization, overlays, resizing, scan commands, selection commands, and exit without early playback stop.
- [`PlaybackNavigationTest.cpp`](../../../test/tui/PlaybackNavigationTest.cpp) protects playback metadata link geometry, title priority, per-field hover, literal artist queries, album-group reveal, navigation history, command parity, and stale or modal pointer input.
- [`PlaybackHoverTest.cpp`](../../../test/tui/PlaybackHoverTest.cpp) protects hover validity against painted geometry and explicit cancellation, including survival of an elapsed-time digit change and a height-only resize.
- [`PlaybackModeTest.cpp`](../../../test/tui/PlaybackModeTest.cpp) protects mode codes, stable control geometry, invalid-mode targets, and localized hover hints under space and input constraints.
- [`PinnedNavigationTest.cpp`](../../../test/tui/PinnedNavigationTest.cpp) protects independent pinning and access, dock and chooser focus, the undocked chooser entry points, divider and disclosure targets, foreground cancellation of scrollbar capture, and layout checkpoints.
- [`LibraryNavigationTest.cpp`](../../../test/tui/LibraryNavigationTest.cpp) protects chooser preorder, indentation, and label contents; [`LibraryControllerTest.cpp`](../../../test/tui/LibraryControllerTest.cpp) protects chooser selection across refresh.
- [`PanelResizeTest.cpp`](../../../test/tui/PanelResizeTest.cpp) protects width constraints, resize commit/rollback, hover retirement, and mode hints; [`PanelDividerTest.cpp`](../../../test/tui/PanelDividerTest.cpp) protects shared/separate border geometry and reveal hover.
- [`TrackDetailLinesTest.cpp`](../../../test/tui/TrackDetailLinesTest.cpp) protects detail row labels, filename fallback, and metadata-title precedence.
- [`DetailSectionsTest.cpp`](../../../test/tui/DetailSectionsTest.cpp) protects detail focus, section input, the focused Credits heading, and activity/hint allocation; [`DetailFieldLayoutTest.cpp`](../../../test/tui/DetailFieldLayoutTest.cpp) protects aligned values, localized filename labels at constrained widths, and Unicode wrapping without changing explicit line breaks.
- [`GoToMenuTest.cpp`](../../../test/tui/GoToMenuTest.cpp) protects navigation suffixes, prefix rebinding, input ownership, history, menu/footer click parity, disabled targets, stale state, and localized hints at constrained widths.
- [`ExitControllerTest.cpp`](../../../test/tui/ExitControllerTest.cpp) protects exit phase-before-output, reentrancy, and one exit publication.
- [`LibraryScanControllerTest.cpp`](../../../test/tui/LibraryScanControllerTest.cpp) protects single-flight scan cancellation, late-result suppression, and the production eager-scan binding.
- [`ListAuthoringControllerTest.cpp`](../../../test/tui/ListAuthoringControllerTest.cpp) and [`SmartListEditorTest.cpp`](../../../test/tui/SmartListEditorTest.cpp) protect Saved-List authoring targets, preview debouncing, submission and deletion lifetime, and the editor surface; see the [TUI List-authoring specification](tui-list-authoring.md).
- [`MouseBindingsTest.cpp`](../../../test/tui/MouseBindingsTest.cpp) protects painted shortcut bindings and clipped row hit regions.
- [`TuiSignalProbeTest.cpp`](../../../test/tui/TuiSignalProbeTest.cpp) drives [`ao_tui_signal_probe`](../../../test/fatal/TuiSignalProbeScenario.cpp) to protect watcher signal routing and previous-handler restoration outside the ordinary unit-test process.
- [`LibraryControllerTest.cpp`](../../../test/tui/LibraryControllerTest.cpp) protects exact restored-view attachment, valid empty projections, reload preservation, restored custom presets, list-deletion recovery, and mark/range/select-all reconciliation.
- [`TerminalTrackColumnLayoutTest.cpp`](../../../test/tui/TerminalTrackColumnLayoutTest.cpp), [`TrackTableTest.cpp`](../../../test/tui/TrackTableTest.cpp), and [`LayoutStateStoreTest.cpp`](../../../test/tui/LayoutStateStoreTest.cpp) protect terminal-cell projection, sections, viewport, persisted widths, and selection.
- [`ListNavigationModelTest.cpp`](../../../test/tui/ListNavigationModelTest.cpp) protects shared-tree preorder adaptation, expansion, cursor identity, and local search.
- [`NavigationPanelTest.cpp`](../../../test/tui/NavigationPanelTest.cpp) protects docking budgets, workspace focus, and painted navigation targets.
- [`WorkspaceUsabilityTest.cpp`](../../../test/tui/WorkspaceUsabilityTest.cpp) protects narrow workspace guidance, filter recovery controls, and playback text.
- [`RenderTest.cpp`](../../../test/tui/RenderTest.cpp), [`PlaybackPanelTest.cpp`](../../../test/tui/PlaybackPanelTest.cpp), and [`HitRegionsTest.cpp`](../../../test/tui/HitRegionsTest.cpp) protect rendering and hit geometry. [`QualityPanelTest.cpp`](../../../test/tui/QualityPanelTest.cpp) and [`OutputDevicePanelTest.cpp`](../../../test/tui/OutputDevicePanelTest.cpp) protect panel content, localization, and terminal bounds.
- [`CommandCompletionTest.cpp`](../../../test/tui/CommandCompletionTest.cpp) protects prefix, alias, localized action discovery, presentation, Quick-filter, and expression completion, including interior filter ranges.

## Related documents

- [Presentation architecture](../presentation/README.md)
- [Interactive session lifecycle architecture](../session-lifecycle.md)
- [Track-column layout](../presentation/track-column-layout.md)
- [List-navigation tree](../presentation/list-tree.md)
- [Activity status](../presentation/activity-status.md)
- [TUI track authoring](tui-track-authoring.md)
- [TUI List authoring](tui-list-authoring.md)
- [TUI command reference](../../reference/tui/command.md)
- [Linux MPRIS system-media integration](mpris.md)
