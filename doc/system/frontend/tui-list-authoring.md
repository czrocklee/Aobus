---
id: tui.list-authoring
---

# TUI List authoring

## Scope

This specification owns the terminal frontend's Saved-List authoring surface after the shell chooses a target List: the definition editor for `new` and `edit`, preview recomputation, submission outcomes, and the lifetime of an admitted save through graceful exit.

The [TUI interaction specification](tui.md) owns command parsing, editor entry, exclusive-input precedence, workspace gesture retirement, and general exit checkpoint ordering.
Exact user-facing commands belong to the [TUI command reference](../../reference/tui/command.md).
Frontend-neutral List mutation, draft, and deletion semantics belong to [library mutation](../library/mutation.md); the shared editor view state, preview status, and Auto presentation resolution belong to the presentation layer's UIModel contracts.

## Targets and actions

`:list new` and `:list edit` are palette-discoverable command aliases, also listed in the shell help.
The target is the Lists-navigation cursor while the docked Lists pane owns keyboard focus, otherwise the active List.
Inside the docked pane, outside search, the workspace's Edit key keeps its verb and edits the List under the cursor; creation has no pane key because it has no focused object, and `n` stays the global notifications key.
The shared `describeListActions` contract decides admissibility: a virtual target refuses edit with one localized warning and changes nothing, while `new` parents at the library root through `parentForNewSmartList`.
Opening any surface runs the same entry transition as the track editors: transient gestures and pointer input retire, a running visual range commits into the mark set, and shell text input closes; the Detail inspector's visibility preference is preserved.

## Definition editor

The editor is a centered modal over the live workspace using a dimmed backdrop, composed per the Track Properties editor pattern.
The workspace remains visible but inert: every keyboard event and mouse gesture that reaches the open editor is consumed, and cover art is neither requested nor painted while it is active.

Rows are Name, Description, and Expression, each a plain single-line text field; an empty Expression shows muted guidance naming the completion prefixes.
Up/Down move field focus, Return advances and wraps, and only the Expression row completes: non-empty typing or `Ctrl-N` requests candidates from the shared query-expression completer in an anchored popup, arrows and page keys navigate it, `Return` accepts the highlighted candidate, and `Esc` dismisses the popup before it means anything about the editor.
`Ctrl-S` always hands the draft to the controller, which recomputes the preview from that exact text before its gate decides: a nonempty name and an expression the live source accepted. The last landed preview may be up to one debounce behind in either direction, so it only dims the Save chip and never decides a submission.
`Esc` closes a clean editor at once and asks one discard question for a dirty draft; `Enter` confirms and `Esc` keeps editing.
The header `×` follows the same `Esc` protocol, and field rows and candidates are clickable.

Below the fields the editor shows the shared view state: the preview status line, the bounded list of leading matches rendered with the shared track-label formatter, the membership line, and the source's own diagnostic when the expression was rejected.
The matches are a sample in source order, as in the GTK preview; the saved List then shows them in its resolved presentation.
On a short terminal the sample is the part that yields: it ends in an ellipsis row, while fields, the completion popup, status, guidance, diagnostic, and footer keep their rows; when not even that row is left, the sample gives way entirely and the status line still states the match count.
The inherited and effective expressions and each sampled track keep one row, ending in an ellipsis when the value is wider than the modal.
An invalid expression hides the preview and withholds submission until it parses and evaluates again.

The preview recomputes on the callback executor from a transient projection over a track source acquired for the target's parent and the draft's local expression, so status, diagnostic, and sample rows describe exactly what saving would evaluate.
Each field edit replaces a pending generation of one 200-millisecond debounce, exactly like the Quick Filter debounce; a superseded generation never lands, and submission or closure cancels the pending one.
When the parent source cannot be acquired, the editor reports the runtime's diagnostic and withholds submission rather than previewing or writing against a parent that is gone.

## Submission and outcomes

`Ctrl-S` builds one `ListDraft` from the field values and submits it through the shared List authoring command: creation for `new`, definition update for `edit`.
The submission task is created before any in-flight flag is armed, one submission can be pending at a time, and a submitting editor consumes all dismissal and editing input.

The outcomes are presented as follows:

- Success closes the editor. A new List resolves the Auto track presentation from the saved expression, records it in the per-List presentation preferences, and is opened, as the GTK sidebar selects it, so the created List on screen confirms the save; an edit keeps its stored presentation and leaves the workspace where it was. No extra notification claims the change.
- A recoverable `Result` error keeps the draft and its binding and shows the runtime's diagnostic in the editor footer; `Ctrl-S` retries, because a List save carries no retained session that could go stale.
- Cancellation restores the editable draft and reports the same retryable diagnostic; retirement suppresses all late presentation.

A submitted save outlives the editor that started it: closing or retiring the surface keeps the shared controller state alive until the write settles, and `ExitController` waits for it exactly as it waits for a submitted track write.
Retirement suppresses late presentation but never cancels the write.

## Implementation map

- [`ListAuthoringController.h`](../../../app/tui/ListAuthoringController.h) defines the single-flow controller and its outputs; [`ListAuthoringController.cpp`](../../../app/tui/ListAuthoringController.cpp) owns target admission, preview recomputation, and submission lifetime.
- [`SmartListEditor.h`](../../../app/tui/SmartListEditor.h) defines the modal editor's modes, status, and requests; [`SmartListEditor.cpp`](../../../app/tui/SmartListEditor.cpp) owns field rows, expression completion, discard confirmation, and preview rendering.
- [`EventController.cpp`](../../../app/tui/EventController.cpp) owns the command aliases' target rule and the entry transition; [`App.cpp`](../../../app/tui/App.cpp) composes the controller, its modal, and the exit gate.

## Test map

- [`ListAuthoringControllerTest.cpp`](../../../test/unit/tui/ListAuthoringControllerTest.cpp) protects target admission, preview debounce generations, fast-save revalidation in both directions, create and update outcomes, the Auto presentation record, opening a created List, retryable failures, and admitted-write lifetime through retirement.
- [`SmartListEditorTest.cpp`](../../../test/unit/tui/SmartListEditorTest.cpp) protects field editing, submit requests, completion placement and completed-token dismissal, the expression guidance, discard confirmation, submitting inertia, column alignment, the shrinking sample, rendering, and mouse ownership.
- [`CommandTest.cpp`](../../../test/unit/tui/CommandTest.cpp) protects the exact command aliases; [`EventControllerTest.cpp`](../../../test/unit/tui/EventControllerTest.cpp) protects command routing, the Lists-cursor target rule, the Lists pane's Edit key, and a save refused behind an in-flight order write.

## Related documents

- [TUI interaction](tui.md)
- [TUI track authoring](tui-track-authoring.md)
- [TUI command reference](../../reference/tui/command.md)
- [Library mutation](../library/mutation.md)
