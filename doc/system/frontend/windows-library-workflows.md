# Windows library workflows

## Scope

This specification owns WinUI's native library-task workflows: active-session
scan, YAML import/export, List creation and editing, deletion preview, direct
membership, saved order, Quick Filter List creation, committed-tree refresh,
and modal admission and teardown.

Shared [scan](../library/scan-and-identity.md), [YAML transfer](../library/yaml-transfer.md), [List mutation](../library/mutation.md), and [saved-order](../presentation/list-order-authoring.md) semantics remain with Runtime and UIModel.
The [Windows desktop shell](windows.md) owns the window/session lifetime, shell
presentation, track table and Properties, playback, output, theme, Soul, and
SMTC. Modern and Classic are presentations of the same window and workflows;
this page does not split them into separate contracts.

## Shared workflow and modal admission

One window-owned List coordinator presents at most one List editor or deletion
preview. One window-owned transfer coordinator presents at most one mode dialog,
picker, preview, or transfer workflow. Each independently opened List dialog receives
a fresh callback-admission generation and cancellable task lifetime; closing it terminalizes that scope, and a later dialog never reuses it. A transfer workflow is never reused after window retirement.

Before opening any List-authoring, library-transfer, or track-Properties modal,
the window cross-queries all three owners. Only one `ContentDialog` is admitted on a `XamlRoot`. While any such workflow is active, the window absorbs
`Alt+Left`, `Alt+Right`, and mouse back/forward without moving workspace history
and admits no other modal workflow. Those history gestures cannot navigate beneath
an open modal surface; editor targets remain the selection captured at entry.

Properties' Credits editor is an inline child of that already admitted dialog, not a second modal workflow.
Detail category/all-kind actions route through the same Properties owner with a captured scope.
Only one child is active; accepting it updates the parent draft, while parent Save remains blocked until it finishes, including metadata- or tag-only Save routes.
Confirmed Reload replaces the complete draft/binding for the captured targets without nesting another `ShowAsync`; declining discard retains the old draft.
The [Windows Properties contract](windows.md#commands-and-transitions) owns controls and composed submission.

A generation token reports admission only; it does not retain window, session,
coordinator, dialog, Runtime, or selection memory. Every owner retires its gate
before cancellation and remains alive until already admitted dispatcher work
settles. Window retirement closes or hides native confirmation where possible,
cancels the active picker and task lifetime, and suppresses every late
completion before releasing Runtime borrowers.

## Scan

When a successor root has the [database marker](../desktop-library-lifecycle.md#invariants) and the request carries no scan intent, WinUI opens it without an
implicit scan, even if indexing is incomplete. A request carrying scan intent still scans. Without the marker, the successor first activates
the window and settles its durable-root gate, then starts the ordinary initial
scan. The status line, which only the Classic preset shows, carries the
warnings raised while the library opens: a failed root commit, settings or
shortcuts a newer version saved, and a rejected theme. They take it back from
the settled initial scan unless the scan outcome is an Error; without an initial
scan they stay until another status replaces them. Their retained notifications
keep them in the activity feed either way. Initial-scan failure leaves the selected root active and permits a later
**Rescan**.

Rescan runs the transactional scan workflow against the active
`LibrarySession`. Committed changes reach projections only through
`LibraryChanges`. Worker completion resumes on the Runtime callback executor
before changing operation state or publishing status to XAML. Rescan controls
are disabled during the scan, including in a shell rebuilt while it runs. A
direct command still reports the current operation; it does not queue a retry.
Rescan becomes available after the result is reported. A queued Open Library
transition rejects another Open Library request; parent teardown may cancel an
active scan.

Session teardown retires callback admission before requesting scan cancellation.
The retired token suppresses later presentation, while Runtime join keeps the
session and callback dependencies alive through settlement. Scan planning or
application failure remains visible and retryable and does not roll back the
active root.

## Import and export

**Import Library Data** and **Export Library Data** are available from Modern's
More menu and Classic's File menu. Modern's More menu owns global library and
shell actions; its separate Playback options menu contains Stop and Reveal
Current Track. The two menus have distinct accessible names.

Export first selects `delta`, `metadata`, `full`, or `list-only`, defaulting to
`full`, then uses the Windows save picker for a `.yaml` or `.yml` path. Import
first selects `merge` or `restore`, defaulting to `merge`, then uses the Windows
open picker. Both submit to the active session's `LibraryJobs`.

Preparing an import, applying its one-shot plan, and exporting publish coarse
file-named progress through Modern's existing activity surface and report their
terminal outcome through notification and status surfaces. Switching to Classic
does not cancel or restart admitted work; Classic's status bar reports the
terminal result.

A merge applies its prepared plan directly. A restore instead presents payload
version and mode, target scope, create/update/delete counts, and the counts of
ignored references and kept invalid List filters. Its destructive action is
scope-specific, defaults to Cancel, and is the only route that applies the plan.
Rejecting the preview drops that one-shot plan.

Cancelling a mode dialog or picker changes nothing. Malformed YAML and
recoverable picker, preparation, import, or export failure remain visible in
notification history and status without optimistically changing the selected
library. Teardown follows the shared modal and callback-settlement rules above.

## List tree and navigation

Modern's `NavigationView` and Classic's `TreeView` both display All Tracks and the saved-List hierarchy from the shared [List projection](../presentation/list-tree.md). All Tracks is read-only; its context menu can create a root List. A saved List's menu can create a child, edit the List, or delete it (with its subtree if it has descendants).

A sidebar selection passes the [List's preferred or recommended presentation](../presentation/list-preference.md#resolution) to workspace as `NewViewDefault`: a new plain view uses it, a reused one keeps its exact presentation. Browsing never writes or clears the pin. Navigation failure is reported and leaves the active view unchanged.

The tree rebuilds only from committed library reset, List-upsert, and
List-deletion publications. Native replacement suppresses selection publication,
retains surviving expansion, and expands ancestors of the active List. Deleting
the active List or subtree falls back to All Tracks; delayed workspace
observations tolerate the removed prior view. The frontend never publishes an
optimistic tree mutation.

Modern exposes the native navigation-back affordance. Both shells route
`Alt+Left`, `Alt+Right`, and mouse back/forward through shared workspace history,
rereading availability after workspace changes rather than keeping a parallel
stack. Replay restores the recorded presentation without consulting or changing
the pin. Modal workflows apply the admission rule above.

## Create and edit Lists

The native editor captures its target at entry. **Inherited Filter** shows the direct parent's saved local expression; **Effective Filter** combines that expression with the draft's local filter. These labels do not expand the ancestor chain. The preview count, by contrast, uses the parent's actual membership, including all ancestor restrictions. The editor shows no sample rows. It also shows direct-membership capability and presentation choices.

While the draft is open, committed track or List changes that affect the source refresh the count and filter context without requiring a draft edit. If the captured parent disappears, both filter labels report it as unavailable. The unavailable preview is neither an invalid local expression nor a valid zero count.

Create or Save requires a named draft whose current preview allows submission. The button can briefly reflect the previous preview after a filter edit, but activation cancels the pending debounce and refreshes the exact draft before admitting a write.
A named draft with a blank local expression can still be submitted if its preview source is available, even if that source reports an inherited filter error. With a non-empty local expression, a filter error in the saved parent chain blocks submission and appears in the diagnostic.
A rejected preview or write leaves the draft open with its diagnostic and changes neither the target nor the tree. These inline diagnostics are not posted to the feed. If the target is already gone when Edit is invoked, there is no editor: the failure is a retained Error and is also reported to the live status line.

Once the List save succeeds, the editor pins the selected named presentation, or clears the pin for Auto or no valid selection. A stored key for an unavailable custom preset opens as Auto: saving in that state clears the pin; merely browsing to the List does not. A failed List save leaves the preference untouched.

The editor then navigates with an `Override` so the history point records the resolved presentation. Even a rename reapplies the choice to a reused view, replacing a temporary header sort. If navigation fails, the editor reports a retained Error to the feed and the live status line and closes; the saved List and its preference remain. Successful navigation updates the status line without posting an activity notification.

Quick Filter shows **Create List from current filter** beside the native
suggestion box only when shared filter state has a non-empty valid resolved
expression. It opens the ordinary editor beneath the active saved List, or as a
root beneath a virtual source, and seeds the local expression with that resolved
value rather than unresolved Quick text.

## Delete Lists

For a single List, the confirmation shows its name. For a subtree, it shows the names and stable List IDs found at preview time. The [deletion command](../library/mutation.md#list-deletion) resolves the subtree and any requested tag cleanup again when it runs; the preview does not freeze either. Removing the root List's directly writable tag from affected tracks is unchecked by default; nested Playlists' tags are kept. Rejecting the preview deletes nothing. Cancellation does not undo a durable commit; only committed deletion updates the tree and triggers active-view fallback.

A failed deletion preview has no confirmation surface. It reports a retained Error to the feed and the same diagnostic to the live status line, as does a failed fallback to All Tracks after deletion. A deletion write failure instead stays in the open confirmation, without a duplicate feed post.

## Membership and saved order

The row menu submits the captured track selection through `ListMembershipAuthoringSession`. **Add to Playlist** lists other directly writable Playlists by name and serialized tag expression; the command uses the List ID, not that label. The active writable Playlist offers removal instead. If it is the only writable List, Add shows a disabled empty item while Remove stays available.

Membership completion posts one message to the notification feed and, while the window is still live, the same text to the shell status line. Applied and NoOp are transient Info, Busy is transient Warning, and Stale and Unavailable are retained Errors. A binding or command error retains its own diagnostic. Classic displays the status line rather than the activity strip; replacing that line does not erase a retained notification. Modern displays the feed through the [activity strip](../presentation/activity-status.md): an active task, warning, or error can take priority over a transient success message. That message is not replayed when the strip becomes free.

The same captured selection exposes Manual Order **Move Up**, **Move Down**,
**Move to Top**, **Move to Bottom**, and **Reset Order** according to
`ListOrderAuthoringSession` capability flags. The four movement handlers also
receive `Alt+Up`, `Alt+Down`, `Alt+Home`, and `Alt+End`. They are native component
actions, not additions to the layout-document action schema. WinUI does not
expose drag reordering or Forget Hidden Positions. Classic shows
move and reset results in its status line. When the tree restores the already-active List, its
selection callback leaves the operation status intact. Selecting a different
List reports navigation instead.

A Manual Order command posts its result once and reports the same text to the live status line. A refusal to start and all writer statuses (Applied, NoOp, Busy, Stale, Unavailable) are transient Info; an error returned by the submitted command keeps its diagnostic as a retained Warning. Delivery to the feed does not guarantee that the compact strip displays the message: the same activity priorities apply as for membership.

Membership and order continuations use window or workflow-generation admission.
Retired admission suppresses late presentation; neither selection changes nor a later dialog retarget the submitted operation.

## Persistence

List editor drafts, deletion previews, transfer plans, picker state, and dialog
state are not persisted. Accepted List definitions, tags, membership, and saved
order use their normal library mutation contracts. Per-List presentation and
column preferences use shared `trackView.presentations` and
`trackView.columnLayouts`; the [workspace session](../workspace/session.md)
separately captures each live view's exact presentation, including its sort.
A failed preference-file write is logged and does not roll back the List save or
in-memory preference. Windows desktop paths and schema details
remain in the [Windows desktop state reference](../../reference/windows/desktop-state.md).

## Implementation map

- [`LibrarySession`](../../../app/windows-winui/app/LibrarySession.h) owns the
  active-session scan workflow using shared
  [`runLibraryScanAsync`](../../../app/include/ao/uimodel/library/task/LibraryScanOutcome.h).
- [`LibraryTransferCoordinator`](../../../app/windows-winui/library/LibraryTransferCoordinator.h)
  owns native mode dialogs, pickers, restore confirmation, and transfer lifetime;
  [`LibraryTransferAdapter`](../../../app/windows-winui/include/ao/winui/library/LibraryTransferAdapter.h)
  maps selector rows and reports without WinRT types.
- [`ListAuthoringCoordinator`](../../../app/windows-winui/list/ListAuthoringCoordinator.h)
  owns List CRUD, deletion preview, membership, and order;
  [`ListAuthoringAdapter`](../../../app/windows-winui/include/ao/winui/list/ListAuthoringAdapter.h)
  owns committed-tree invalidation and restoration policy without WinRT types.
- [`TrackQuickFilterControl`](../../../app/windows-winui/track/TrackQuickFilterControl.h)
  and `track.quickFilter` in
  [`TrackComponents.cpp`](../../../app/windows-winui/layout/component/track/TrackComponents.cpp)
  own completion and valid-expression List creation.
- [`NavigationPane`](../../../app/windows-winui/layout/component/shell/NavigationPane.cpp)
  adapts the List tree and workspace history. `MainWindow` owns cross-workflow
  modal admission and the common `XamlRoot`.

## Test map

- [`LibraryScanWorkflowTest.cpp`](../../../test/uimodel/library/task/LibraryScanWorkflowTest.cpp)
  protects the scan decision shared by GTK and WinUI.
- [`LibraryTransferAdapterTest.cpp`](../../../test/winui/library/LibraryTransferAdapterTest.cpp)
  protects selector mappings, restore-only confirmation, and complete preview
  text; shared task-service and YAML tests protect execution and data semantics.
- [`ListAuthoringAdapterTest.cpp`](../../../test/winui/list/ListAuthoringAdapterTest.cpp)
  protects presentation resolution, committed invalidation, expansion,
  ancestor reveal, and fallback; shared List editor, membership, and order tests
  protect semantic authoring.
- The [native automation probe](../../../test/winui/probe/WinUiAutomationProbe.cpp) checks Create before a filter edit's debounce fires, and the open editor's labels, count, and button after a parent filter change and deletion.
- [`WinUiTrackTableProbe.cpp`](../../../test/winui/probe/WinUiTrackTableProbe.cpp) checks List commands against the status callback and the real feed, including severity, lifetime, and retirement during a post. It also reads the blocking reason in the loaded Modern activity component on an idle feed. It drives the coordinator directly, not a physical shortcut.
- [`KeymapAcceleratorPlanTest.cpp`](../../../test/winui/input/KeymapAcceleratorPlanTest.cpp)
  protects native-only order accelerators without extending the layout schema.
- [`CallbackAdmissionGateTest.cpp`](../../../test/winui/app/CallbackAdmissionGateTest.cpp)
  proves idempotent retirement and that renewal cannot re-admit an old token.
- Native Debug and Release WinUI builds are still required for XAML dialogs,
  Windows pickers, generated C++/WinRT, and native cancellation behavior.

## Related documents

- [Windows desktop shell](windows.md)
- [Desktop library lifecycle](../desktop-library-lifecycle.md)
- [Interactive session lifecycle](../session-lifecycle.md)
- [Windows desktop state reference](../../reference/windows/desktop-state.md)
- [Use the Windows desktop](../../user/use-windows-desktop.md)
