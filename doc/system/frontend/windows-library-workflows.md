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

A generation token reports admission only; it does not retain window, session,
coordinator, dialog, Runtime, or selection memory. Every owner retires its gate
before cancellation and remains alive until already admitted dispatcher work
settles. Window retirement closes or hides native confirmation where possible,
cancels the active picker and task lifetime, and suppresses every late
completion before releasing Runtime borrowers.

## Scan

When a successor root contains the canonical database, WinUI opens it without an
implicit scan. When no canonical database exists, the successor first activates
the window and settles its durable-root gate, then starts the ordinary initial
scan. Initial-scan failure leaves the selected root active and permits a later
**Rescan**.

Rescan runs the transactional scan workflow against the active
`LibrarySession`. Committed changes reach projections only through
`LibraryChanges`. Worker completion resumes on the Runtime callback executor
before changing operation state or publishing status to XAML. Another Rescan
reports the current operation rather than starting a competing scan. A queued
Open Library transition rejects another Open Library request; parent teardown
may cancel an active scan.

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

Export first selects `delta`, `metadata`, `full`, or `listOnly`, defaulting to
`full`, then uses the Windows save picker for a `.yaml` or `.yml` path. Import
first selects `merge` or `restore`, defaulting to `merge`, then uses the Windows
open picker. Both submit to the active session's `LibraryJobs`.

Preparing an import, applying its one-shot plan, and exporting publish coarse
file-named progress through Modern's existing activity surface and report their
terminal outcome through notification and status surfaces. Switching to Classic
does not cancel or restart admitted work; Classic's status bar reports the
terminal result.

A merge applies its prepared plan directly. A restore instead presents payload
version and mode, target scope, create/update/delete counts, and ignored-reference
count. Its destructive action is scope-specific, defaults to Cancel, and is the
only route that applies the plan. Rejecting the preview drops that one-shot plan.

Cancelling a mode dialog or picker changes nothing. Malformed YAML and
recoverable picker, preparation, import, or export failure remain visible in
notification history and status without optimistically changing the selected
library. Teardown follows the shared modal and callback-settlement rules above.

## List tree and navigation

Navigation exposes Folders, Albums, Artists, Genres, and Playlists in both
shells. Albums, Artists, and Genres apply matching built-in presentations to All
Tracks; Playlists contains the shared List-tree projection. Built-in entries are
read-only. All Tracks and saved-List context menus can create a root or derived
List. Saved Lists additionally expose Edit and either single-node or subtree
Delete according to descendants.

The tree rebuilds only from committed library reset, List-upsert, and
List-deletion publications. Native replacement suppresses selection publication,
retains surviving expansion, and expands ancestors of the active List. Deleting
the active List or subtree falls back to All Tracks; delayed workspace
observations tolerate the removed prior view. The frontend never publishes an
optimistic tree mutation.

Modern exposes the native navigation-back affordance. Both shells route
`Alt+Left`, `Alt+Right`, and mouse back/forward through shared workspace history,
rereading availability after workspace changes rather than keeping a parallel
stack. Modal workflows apply the admission rule above.

## Create and edit Lists

The native editor captures its target when opened and displays inherited and
effective expressions, direct-membership capability, a live result preview, and
the shared presentation catalog. It validates the complete draft before enabling
Create or Save. Recoverable validation, maintenance, stale-binding, or storage
failure leaves the draft open with a visible error and does not retarget the
captured selection or mutate the tree optimistically.

After successful save, WinUI navigates to the authored List and applies its newly
saved explicit presentation or resolved automatic recommendation, even if that
List was already active. Accepted List definitions use normal library mutation;
the accepted presentation choice uses the shared per-List presentation store
rather than becoming a List field.

Quick Filter shows **Create List from current filter** beside the native
suggestion box only when shared filter state has a non-empty valid resolved
expression. It opens the ordinary editor beneath the active saved List, or as a
root beneath a virtual source, and seeds the local expression with that resolved
value rather than unresolved Quick text.

## Delete Lists

Deletion first obtains the shared impact preview and lists every node in a
cascade. Removal of a directly writable tag from affected tracks is unchecked by
default. Rejecting the preview performs no deletion. An admitted operation uses the shared authoring binding and mutation rules; cancellation does not undo a durable commit. Only committed deletion publication rebuilds the tree and triggers deterministic active-view fallback.

## Membership and saved order

The selected-row context menu lists writable tag-backed Playlists by stable List
id and adds the captured stable track selection through
`ListMembershipAuthoringSession`. It offers explicit removal when the active List
itself is directly writable and never infers editability from List name or
presentation.

The same captured selection exposes Manual Order **Move Up**, **Move Down**,
**Move to Top**, **Move to Bottom**, and **Reset Order** according to
`ListOrderAuthoringSession` capability flags. The four movement handlers also
receive `Alt+Up`, `Alt+Down`, `Alt+Home`, and `Alt+End`. They are native component
actions, not additions to the layout-document action schema. WinUI does not
expose drag reordering or Forget Hidden Positions in this version.

Membership and order continuations use window or workflow-generation admission.
Retired admission suppresses late presentation; neither selection changes nor a later dialog retarget the submitted operation.

## Persistence

List editor drafts, deletion previews, transfer plans, picker state, and dialog
state are not persisted. Accepted List definitions, tags, membership, and saved
order use their normal library mutation contracts. Per-List presentation and
column preferences use shared `trackView.presentations` and
`trackView.columnLayouts`; workspace owns the active view's current presentation
and sorting. Windows desktop paths and schema details remain in the
[Windows desktop state reference](../../reference/windows/desktop-state.md).

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

- [`LibraryScanWorkflowTest.cpp`](../../../test/unit/uimodel/library/task/LibraryScanWorkflowTest.cpp)
  protects the scan decision shared by GTK and WinUI.
- [`LibraryTransferAdapterTest.cpp`](../../../test/unit/winui/library/LibraryTransferAdapterTest.cpp)
  protects selector mappings, restore-only confirmation, and complete preview
  text; shared task-service and YAML tests protect execution and data semantics.
- [`ListAuthoringAdapterTest.cpp`](../../../test/unit/winui/list/ListAuthoringAdapterTest.cpp)
  protects presentation resolution, committed invalidation, expansion,
  ancestor reveal, and fallback; shared List editor, membership, and order tests
  protect semantic authoring.
- [`KeymapAcceleratorPlanTest.cpp`](../../../test/unit/winui/input/KeymapAcceleratorPlanTest.cpp)
  protects native-only order accelerators without extending the layout schema.
- [`CallbackAdmissionGateTest.cpp`](../../../test/unit/winui/app/CallbackAdmissionGateTest.cpp)
  proves idempotent retirement and that renewal cannot re-admit an old token.
- Native Debug and Release WinUI builds are still required for XAML dialogs,
  Windows pickers, generated C++/WinRT, and native cancellation behavior.

## Related documents

- [Windows desktop shell](windows.md)
- [Desktop library lifecycle](../desktop-library-lifecycle.md)
- [Interactive session lifecycle](../session-lifecycle.md)
- [Windows desktop state reference](../../reference/windows/desktop-state.md)
- [Use the Windows desktop](../../user/use-windows-desktop.md)
