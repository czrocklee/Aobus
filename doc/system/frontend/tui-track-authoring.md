---
id: tui.track-authoring
---
# TUI track authoring

## Scope

This specification owns the terminal frontend's full Properties editor and tags-only editor after the shell chooses an effective track selection.
It defines captured-target and revision binding, coherent preparation, modal input ownership, editor pages, metadata completion and validation, tag intent, stale/reload behavior, submission outcomes, and the lifetime of an admitted write through graceful exit.

The [TUI interaction specification](tui.md) owns formation and reconciliation of focus, marks, and visual selection; editor entry; workspace gesture retirement; exclusive-input precedence; and general exit checkpoint ordering.
Exact user-facing shortcuts and commands belong to the [TUI command reference](../../reference/tui/command.md).
Frontend-neutral patch and authoring-session semantics belong to the [metadata-editing specification](../presentation/metadata-editing.md), while runtime binding, atomicity, and outcome semantics belong to [library mutation](../library/mutation.md).

## Targets and coherent preparation

The full Properties entry (`e`, `:edit`, or `:properties`) and quick tag entry (`t` or `:tags`) each ask `TrackEditController` to open over the `selectedTrackIds()` supplied by the interaction layer.
The controller captures that vector once.
Nothing during the editor's lifetime adds, removes, or reorders those ids; changing the target set requires closing and opening a new editor.

Preparation is one attempt on the callback executor.
It begins one `TrackAuthoringSession` over the complete captured vector, then reads every required field, target identity, tag count, and suggestion from one `LibrarySnapshot`.
The snapshot revision must equal the session's bound revision, so the displayed baseline describes the exact revision through which the draft can write.
Tags-only preparation omits the metadata/property form but retains target identities and tag membership.

An empty selection, a bind failure, and a revision mismatch all refuse without opening, each posting its own Warning.
A bind-time missing target reports the incomplete selection rather than a generic availability refusal; a target missing from the matching snapshot also refuses the complete selection.
A refusal changes nothing, and another open is refused while an editor or its prior submitted write is active.

The shell's [entry transition](tui.md#track-authoring-entry) retires transient gestures and completes any visual range without changing the captured target vector.

## Modal ownership and rendering

The full editor is composed as a centered modal over the live workspace using a dimmed backdrop.
The workspace remains visible but inert: every keyboard event and mouse gesture that reaches the open editor is consumed, including keys the editor does not use.
No root action, overlay toggle, or mouse gesture can run behind geometry the user cannot see.
The modal fills an `80x24` terminal and leaves the workspace visible around its bounded box on larger terminals.

Cover art is neither requested nor painted while an editor is active.
Kitty artwork is written outside FTXUI's cell buffer after frame flush, so the Kitty owner removes its image while an editor is active or exit is in progress instead of allowing it to paint over the modal.

Editor tabs, metadata fields, tag rows, completion candidates, and footer actions have rendered mouse targets.
Clicking inside an editable value places the caret at a grapheme boundary.
Wheel input navigates the active page or its completion list.
Mouse footer actions use the same confirmation, validation, and submission protocol as their keys; stale or submitting drafts cannot bypass those gates.
The header × control follows the same `Esc` protocol: it dismisses active completion or a nonempty tag query before requesting editor closure.
A dimmed Apply control consumes a click without requesting submission.
The read-only Properties and Tracks bodies support wheel scrolling and Up/Down, j/k, PageUp/PageDown, and Home/End navigation without row click actions; page movement uses the rendered viewport. Their tabs and footer controls remain clickable.

The full editor consumes outside clicks without dismissing itself.
Closing a clean full editor is immediate.
Closing or reloading a dirty full editor first asks for confirmation, and declining preserves the draft.
Confirmation and diagnostic text wraps to the modal width; recovery shortcuts occupy their own row so a long translation cannot truncate the key names.

## Full Properties editor

The full editor organizes authoring into `Metadata`, `Tags`, read-only `Properties`, and, for multi-track selections, `Tracks` pages.
`Tab` and `Shift-Tab` cycle forward and backward through available pages from any control, search input, or completion popup.
Page changes preserve metadata and tag draft state while retiring page-local completion or query state.
Page editors retain their own baseline and draft values rather than borrowing the enclosing modal's members.

### Metadata editing and validation

Metadata editing uses direct keyboard input without checkboxes.
A passive changed indicator (`*`) marks edited rows, while an active indicator (`>`) highlights the focused row.
Typing into a field marks it for replacement; typing a common field back to its baseline removes the pending change.
Deleting an edited field to empty marks it for explicit clear.
For mixed-value fields, empty input remains untouched mixed preservation until an edit is made: `Ctrl-D` records an explicit clear across all targets, while `Ctrl-G` restores baseline or mixed preservation.
`Ctrl-U` and `Ctrl-K` delete text before or after the caret, and `Ctrl-W` deletes the preceding ASCII-space-delimited word.
Those edits preserve an untouched mixed field when its input is already empty; explicit clearing remains `Ctrl-D`.

Inline numeric validation uses the field codec, flags parsing errors, and disables save while preserving the raw draft.
Single-line values refuse control characters, U+2028, and U+2029 rather than sanitizing them, one `insert` call at a time.
FTXUI implements no bracketed paste, so a pasted string arrives as ordinary key events with its newlines as `Return` and cannot be refused as one unit.
A draft can be submitted only when it has effective intent and every included metadata field is valid.

### Metadata completion

Artist, Album, Album Artist, Genre, Composer, Conductor, Ensemble, Work, Movement, and Soloist query the runtime `CompletionService` synchronously on the event thread.
Non-empty typing or `Ctrl-N` requests candidates in an anchored popup.
`Up`/`Down` and `PageUp`/`PageDown` navigate candidates; `Enter` replaces the targeted field text through checked range replacement with `tryReplaceRange`; and `Esc` closes completion without dismissing the editor.

The candidate window contains six rows.
Arrow navigation moves the window only when selection leaves it; page navigation advances both selection and window by six rows, clamped at either end.
The metadata viewport follows the selected candidate while completion is open, including in a terminal too short to show the entire popup.
Clicking a candidate accepts that candidate through the same replacement path.

Every chord the popup declines—`Ctrl-S`, `Ctrl-R`, `Ctrl-D`, `Ctrl-G`, `Tab`, and `Shift-Tab`—closes it before the editor acts, so no confirmation prompt or switched page is drawn under candidates that still appear to own input.
Caret navigation (`Left`, `Right`, `Home`, `End`, `Ctrl-A`, `Ctrl-E`, `Alt-B`, `Alt-F`, `Ctrl-Left`, and `Ctrl-Right`) also closes completion rather than leaving candidates attached to an obsolete caret range.

### Tags page

Every tag row starts with a three-state box showing what all captured targets would carry after submission: `[x]` for every target, `[ ]` for none, and `[~]` for only some.
A pending intent moves and colours the box immediately and names the change as `Add` or `Remove`.
Because the box shows destination rather than origin, only partly carried tags also show the `carried/captured` fraction.

Library tags that no captured target carries follow the selection's own tags.
They need no separate heading because the empty box already reports their selection state.
At most the 50 highest-frequency suggestions are drawn, and truncation reports the number omitted.
The cap applies only to displayed suggestions: draft-marked tags and exact query matches are exempt, so a lower-ranked or already edited tag remains reachable.

The query above the list is always live rather than a second interaction mode.
Printable input, `Backspace`, `Delete`, and caret navigation edit it; `Up`/`Down` and `PageUp`/`PageDown` move result selection.
The query filters the complete library vocabulary by Unicode case-insensitive substring using `makeUtf8CaselessKey`; it is not part of the draft, and `Esc` drops a nonempty query before it means anything about the editor.
A query wider than the modal scrolls horizontally to retain the caret and remains one line tall.

Matching keys normalize to NFC and use Unicode default case folding.
Accepting an existing tag preserves its stored spelling; creating a tag normalizes the new name to NFC without changing case.
A decomposed or case-variant spelling therefore finds an existing tag instead of offering a duplicate row.
Empty or ASCII-whitespace-only queries never offer creation; surrounding spaces in a nonblank new name remain part of its spelling.

`Enter` advances only through intents that would write something:

- when no target carries a tag: `AddToAll`, then baseline;
- when every target carries it: `RemoveFromAll`, then baseline;
- when only some carry it: `AddToAll`, `RemoveFromAll`, then baseline.

A query naming no existing tag offers creation on a trailing row, so result activation prefers an existing match whenever one is selected.
A created tag joins the captured tags rather than the suggestions.
Committing an intent clears the query and keeps selection on that tag; `Ctrl-G` restores the selected tag to baseline.

## Tags-only editor

The quick tag entry uses `TrackEditorMode::Tags` in the same controller, captured session, invalidation, reload, and submission lifecycle as the full editor.
It presents a compact centered popover without the full editor's page strip and excludes metadata from its patch.
When available height is below 12 rows, it omits the single-track subtitle and navigation hints so the query and submission/recovery controls remain available.
Narrow rows prioritize the tag name and intent checkbox over membership count and status text, preserving readable names before and after intent changes.
At 80 columns the maintained locale labels fit on one action row; narrower terminals may stack the actions.

Query and result focus are distinct.
Space toggles a focused result, while spaces entered with query focus remain text.
Arrow or page navigation returns focus to results, and wheel input moves result focus.
`Tab` and `Shift-Tab` switch focus between query and results.

`Enter` applies the pending tag patch.
With query focus it first adds an offered new name even when partial matches also exist; with result focus it creates only from the selected creation row.
An exact search match alone never changes an existing tag.
A clean `Enter` closes without writing.

`Esc`, the exit footer, or an outside left press cancels the draft directly, without the full editor's discard prompt.
The footer says Close for a clean view and Discard changes when tag intents or an offered new tag would be lost.
`Ctrl-R` reloads the same targets only in Failed or Stale state, where the footer advertises recovery; while Ready it preserves the draft.
During submission, all dismissal and editing input remains blocked.

## Stale state and reload

The retained session's invalidation signal is observed on the callback executor.
It changes a Ready editor to Stale, disables Save, and preserves the raw draft.
Any effective library commit can invalidate the session, including a commit unrelated to the captured tracks.
Invalidation while a write is in flight is ignored because that write reconciles its own binding and reports its terminal result.

`Ctrl-R` runs the same one-attempt preparation against the complete captured id vector and retains the current editor mode.
A successful reload replaces the baseline, raw inputs, and session together.
A failed reload preserves the previous draft and reports its diagnostic.
No draft is ever reattached automatically to a fresh session, and no reload retargets the editor to the current workspace selection.

## Submission and outcomes

`Ctrl-S` in the full editor and the apply path in tags-only mode build one `TrackPropertiesPatch` and submit it through the retained session with `submitPropertiesAsync()`.
The full patch combines metadata edits with `tagsToAdd` and `tagsToRemove`; tags-only mode supplies only tag changes.
Entry into the lazy session submission and terminal-result handling both run on the callback executor alongside session invalidation.
Only one submission can be pending, and a submitting editor consumes all dismissal and editing input.

Metadata and tags are one runtime command and transaction.
Changed targets across both parts are deduplicated into one mutation count.
A reply with fewer change records than captured targets means some targets already carried the requested value; it is not partial failure.
The outcomes are presented as follows:

- `Applied` and `NoOp` close the editor and post the applied or no-change result.
- `Busy` preserves the draft and binding and re-enables Save only if the session remains current; otherwise the editor becomes Stale. Its retryable diagnostic clears on the next keyboard event so ordinary shortcuts return.
- `Stale` and `Unavailable` preserve the draft and offer Reload or Close.
- A recoverable operational `Result` error preserves the draft and offers Reload or Close with its own message.
- Cancellation while the editor remains active changes it to Stale and preserves the draft for Reload or Close; it does not claim that a durable commit rolled back.

Pending-submission bookkeeping clears before any presentation work, so a presentation failure cannot leave exit waiting for a write that already settled.

## Admitted-write lifetime and exit

Creating the submission task transfers retention to shared authoring Session State before the editor can close.
Closing the editor during submission therefore keeps the authoring session state alive until the write settles. That state only borrows the runtime `Library`; it does not extend the runtime's lifetime.
Controller retirement suppresses late presentation but never cancels the write.
The controller destructor drops only the editor, invalidation observer, and output callbacks; it does not dispatch, cancel, or present.
The runtime must outlive that admitted operation.

The App-owned `ExitController` asks exactly once, before retirement, whether a submitted Properties write is settling.
There is at most one admitted write, so the handshake needs no count or second pending ledger.
With no pending write, the first request retires scan/editor presentation and transient input, then posts loop exit.
With a pending write, the gate enters `WaitingForSubmittedWrite`: the status row shows the waiting message, ordinary input is consumed before reaching editor or workspace, and retirement removes presentation without cancelling the write.
Write settlement posts loop exit exactly once.
A second exit request ends the application-level wait and posts loop exit exactly once; runtime shutdown may still stop and join work.
A settlement outside the waiting phase is a no-op.

The general exit sources, interaction retirement, persistence checkpoints, playback stop, and destruction ordering remain defined by the [TUI interaction specification](tui.md#exit-and-teardown).

## Implementation map

- [`TrackEditController.h`](../../../app/tui/TrackEditController.h) defines the single-editor controller and pending-submission handoff; [`TrackEditController.cpp`](../../../app/tui/TrackEditController.cpp) owns coherent preparation, retained authoring session, invalidation, reload, submission outcomes, and retirement.
- [`TrackPropertiesEditor.h`](../../../app/tui/TrackPropertiesEditor.h) defines modes, pages, status, owner requests, and patch summaries; [`TrackPropertiesEditor.cpp`](../../../app/tui/TrackPropertiesEditor.cpp) owns full-modal input, confirmation, page composition, validation gating, and combined patch construction.
- [`TrackPropertiesEditorTagPopover.cpp`](../../../app/tui/TrackPropertiesEditorTagPopover.cpp) owns tags-only rendering, query/result focus, dismissal, and local input.
- [`TrackMetadataEditor.cpp`](../../../app/tui/TrackMetadataEditor.cpp) owns metadata rows, explicit intent, codec validation, completion, and read-only Properties projection.
- [`TrackTagEditor.cpp`](../../../app/tui/TrackTagEditor.cpp) owns tag intent, Unicode query matching, suggestion capping, creation, and patch projection.
- [`TrackAuthoringSessions.h`](../../../app/include/ao/uimodel/library/track/TrackAuthoringSessions.h) defines stable targets, bound revision, invalidation, and the retained asynchronous facade; [`TrackAuthoringSession.cpp`](../../../app/uimodel/library/track/TrackAuthoringSession.cpp) owns Session State and result reconciliation.
- [`ExitController.cpp`](../../../app/tui/ExitController.cpp) owns the idempotent pending-write exit gate; [`App.cpp`](../../../app/tui/App.cpp) wires pending-state inspection, settlement notification, waiting input ownership, and status rendering.

## Test map

- [`TrackEditControllerTest.cpp`](../../../test/unit/tui/TrackEditControllerTest.cpp) protects whole-selection preparation and refusal, one active editor, callback-executor submission, unified patches, staleness, reload rebinding, tags-only target retention, and settlement after editor/controller retirement.
- [`TrackPropertiesEditorTest.cpp`](../../../test/unit/tui/TrackPropertiesEditorTest.cpp) protects modal consumption, page focus, mixed-value intent, explicit clear/restore, validation, patch construction, confirmations, submission-state rendering, short-terminal layout, and cross-page draft preservation.
- [`TrackPropertiesEditorCompletionTest.cpp`](../../../test/unit/tui/TrackPropertiesEditorCompletionTest.cpp) protects completion request and acceptance, six-row paging, dismissal ownership, caret invalidation, and selected-candidate visibility in short terminals.
- [`TrackPropertiesEditorTagsTest.cpp`](../../../test/unit/tui/TrackPropertiesEditorTagsTest.cpp) protects three-state intent, Unicode matching and normalization, creation priority, query ownership, suggestion caps, wide queries, and membership counts.
- [`TrackTagPopoverTest.cpp`](../../../test/unit/tui/TrackTagPopoverTest.cpp) protects tags-only patches, query/result focus, direct cancellation, localized Close/Discard transitions, mouse and narrow-layout targets, and submission/recovery controls.
- [`EditorMouseTest.cpp`](../../../test/unit/tui/EditorMouseTest.cpp) protects field, tab, tag, completion, confirmation, and grapheme-position mouse routing.
- [`ExitControllerTest.cpp`](../../../test/unit/tui/ExitControllerTest.cpp) protects pending-state inspection before retirement, wait settlement, second-request override, reentrancy, and one exit publication.
- Shared [`TrackAuthoringSessionTest.cpp`](../../../test/unit/uimodel/library/track/TrackAuthoringSessionTest.cpp) and [`LibraryCommandsTrackPropertiesTest.cpp`](../../../test/unit/runtime/library/LibraryCommandsTrackPropertiesTest.cpp) protect retained submission lifetime, binding advancement/invalidation, combined publication, and rollback.

## Related documents

- [TUI interaction specification](tui.md)
- [TUI command reference](../../reference/tui/command.md)
- [Metadata-editing specification](../presentation/metadata-editing.md)
- [Library access and mutation](../library/mutation.md)
- [Interactive session lifecycle architecture](../session-lifecycle.md)
