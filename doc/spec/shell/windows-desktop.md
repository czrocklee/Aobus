---
id: shell.windows-desktop
type: spec
status: current
domain: application-shell
summary: Defines the native Windows desktop shell, mode switching, destructive library restart, responsive layout, and Windows integration behavior.
---
# Windows desktop shell

## Scope

This specification owns the observable behavior of the native WinUI 3 desktop frontend. It covers its Modern and Classic shells, shared session lifetime, WinUI adaptation of library selection, rescan, and YAML transfer, track-list interaction, track and List authoring, Soul behavior, theme reload, and Windows media integration.

It does not redefine the [shared desktop library lifecycle](../application/desktop-library-lifecycle.md), its [private successor arguments](../../reference/application/desktop-successor-protocol.md), library scan semantics, playback engine behavior, or the exact Windows YAML fields. Those remain with their shared owners and the [Windows desktop state reference](../../reference/windows/desktop-state.md).

## Code boundary

The [system architecture](../../architecture/system-overview.md) defines the runtime-to-UIModel-to-frontend dependency direction. The [application shell architecture](../../architecture/application-shell.md) owns shell composition. Cross-desktop root, startup, protocol, and detached-launch rules live in `ao_desktop_launch`; shared presentation policy lives under `app/include/ao/uimodel/`. Windows-only lifecycle effects, policy, XAML, controls, and adapters live in the Windows-only `aobus-winui-lib` under `app/windows-winui/`, while `aobus-winui` is the thin final-link and deployed-resource target.

## Terminology

- **Library session**: the library-bound owner of one runtime, settings, scan workflow, and playback-command surface; one process-lifetime `LibraryWindowSession` owns it together with its `MainWindow`.
- **Modern**: the default integrated-title-bar shell with navigation, track list, inspector, and persistent Now Playing area.
- **Classic**: the system-title-bar, high-density shell with menus, toolbar, library tree, property panel with artwork, status bar, and GTK-compatible playback-strip order.

## Invariants

- One process owns at most one `LibrarySession`, one unique runtime, and one main window; opening a different root replaces the process rather than adding another graph.
- A different-root restart prepares the still-live graph by checkpointing state and terminally retiring playback persistence. Retirement failure leaves that graph usable and launches no successor.
- The successor does not restore playback or admit playback writes until its selected root is durable. A failed root commit preserves the prior live settings snapshot and permanently seals playback writes.
- Switching shells changes presentation only; it neither scans the library nor interrupts playback or an admitted library task.
- The current output route remains Runtime state. Both shells select it through
  the same UIModel output-device model used by GTK and TUI.
- All runtime and UIModel callbacks that touch XAML state execute through the window dispatcher.
- Replacing track-table items suppresses native selection publication, restores the Runtime-owned selection, and keeps the top visible track anchored by stable id whenever it survives the replacement.
- The window detaches its observers and controllers before its session destroys the unique runtime.
- Modern is the first-run shell. Both shells follow the effective Windows light or dark theme unless semantic theme tokens override their surfaces.
- Authored Windows shell copy, dynamic status templates, presentation labels, and column labels resolve through the WinUI resource system. Library metadata, device names, paths, and diagnostics supplied by lower layers remain data rather than resource identifiers.
- The track table remains usable at every supported width. The inspector collapses before navigation; narrow layouts also collapse navigation.
- Soul radius, anchor geometry, brand colors, aura interpretation, gradient stops, and animation periods come from shared UIModel constants. Theme YAML cannot replace them.
- A shell mode may apply a positive stroke width and inner-glyph scale to fit its allocated control size without changing those shared brand and motion constants.
- One main window presents at most one track-properties dialog, and the dialog never retargets its captured selection after it opens.
- One window-owned List coordinator presents at most one List editor or deletion-preview dialog, and every independently opened dialog receives a fresh cancellable lifetime.
- One window-owned library-transfer coordinator presents at most one mode dialog, picker, preview, or transfer workflow at a time; it never reuses a workflow after window retirement.

## State model

The shell mode is `modern` or `classic`.
Responsive state is derived from the current client width.
Below 720 effective pixels navigation uses the minimal overlay presentation and the inspector is available as an explicit overlay.
From 720 through 1119 navigation uses the compact presentation and the inspector remains an explicit overlay.
At 1120 or wider navigation is expanded and the inspector is inline.
Below the wide tier the browser summary yields its space to the filter. At the
narrow tier the Now Playing artwork and text yield their space to transport,
time, volume, and overflow commands.

Library reads, playback, runtime resources, commands, and activity status always derive from the same active runtime.
After audio providers are registered, startup resolves and resubmits the stored
desktop output preference before controllers bind. The shared pure UIModel policy
requires non-empty backend and profile ids and rejects a profile known to be
unsupported by a published backend. A non-empty device may be submitted before
catalog publication or while temporarily unavailable so Runtime can retain
pending intent; an empty device is valid only when the published catalog
advertises a compatible empty-id default. WASAPI therefore requires a concrete device id. Intent that cannot be
submitted leaves the stored preference intact and the runtime-selected default
in effect; either outcome is non-fatal.
An ordinary startup starts playback-session observation and restores listening
intent after workspace and providers are ready but before controllers bind. A
successor startup instead begins in `AwaitingRootCommit`; commit success moves
it to ready observation, commit failure moves it to the runtime write seal, and
terminal parent preparation moves it to retired.

Soul frame updates run only while the control is loaded, its app window is
visible and not minimized, and playback requires animation. Window focus alone
does not pause an otherwise visible animation.
Modern applies the compact control-local values `strokeWidth: 5.0` and `glyphScale: 0.85`, and renders the Play/Pause glyph independently of the rotating ring so it remains upright.
Classic renders a 32 by 32 glyph-free Soul with the shared default stroke width independently from its ordinary Play/Pause and Stop buttons.

## Commands and transitions

Selecting Modern or Classic saves the selection and updates title-bar ownership before showing the selected shell. Existing list selection, library data, and playback continue.

Track Details shows the inspector and, given again, hides it. Both shells offer
it: Modern in the browser summary bar, Classic in the View menu. Until it is
given, each presentation answers for itself: an inline inspector shows and an
overlay stays hidden. Once given, the request holds across width changes and
shell selection for as long as the window lives, and is not persisted. A hidden
inspector returns its width to the workspace. A shown overlay covers the
workspace at the inspector's persisted width without taking width from it, does
not take the pointer, and keeps following the selection underneath.

Open Library uses the Windows folder picker initialized from the current `AppWindow` id.
Selecting the already active filesystem directory, including an equivalent
alias, is a no-op.
A different-root request is accepted once and posted to the application dispatcher; the picker coroutine returns before destructive work begins.
The parent captures current window and workspace state best effort and
terminally retires the playback-session payload while the window remains
usable. Retirement failure reports in that window and returns the application
to its running phase. Success retires all window/runtime borrowers, releases
its `MainWindow`, `LibrarySession`, unique runtime, and application-state
stores, and then invokes the shared Boost.Process V2 adapter for the exact
current executable with the paired private successor request.
The parent exits after the launch attempt and never reconstructs its old graph.

The successor treats that explicit root strictly: a missing, inaccessible, non-directory, database, writer-lease, runtime, or window failure is a successor startup failure rather than an empty-library fallback.
It starts playback idle and leaves playback persistence dormant. After its
native window and process-wide playback adapters are active, the successor
saves a desktop-settings candidate containing the selected root. A successful
save installs that candidate and starts playback observation. A failed save
keeps the previous in-memory and durable root, seals playback writes, and keeps
the active target session otherwise usable.
When the selected root already contains the canonical database, it opens without an implicit scan.
When it has no canonical database, the successor becomes active before its ordinary initial scan starts.
An initial-scan failure leaves the selected root active and permits a later Rescan.
Rescan runs the transactional scan workflow against the active session; committed changes reach projections only through `LibraryChanges`.
Its worker completion resumes on the Runtime callback executor before changing session operation state or publishing status to XAML.
There is no live-runtime exchange inside `LibrarySession`.
Open Library may cancel an active scan through parent teardown; an already queued process transition rejects another Open Library request, while an active Rescan still reports its current operation for another Rescan.

Import Library Data and Export Library Data are available from Modern's More and narrow Now Playing overflow menus and from Classic's File menu.
Export first selects `delta`, `metadata`, `full`, or `listOnly`, defaulting to `full`, then uses the Windows save picker for a `.yaml` or `.yml` path.
Import first selects `merge` or `restore`, defaulting to `merge`, then uses the Windows open picker.
Both paths submit to the active session's `LibraryJobs`; preparing an import, applying its plan, and exporting publish coarse file-named progress through Modern's existing activity surface and report their terminal outcome through the existing notification and status surfaces.
Switching to Classic does not cancel or restart an admitted transfer; Classic's status bar reports the terminal status when it completes.
A merge applies its prepared one-shot plan directly.
A restore instead displays the prepared report's payload version and mode, target scope, create-update-delete counts, and ignored-reference count; its destructive action is scope-specific, defaults to Cancel, and is the only path that applies the plan.

Navigation exposes Folders, Albums, Artists, Genres, and Playlists entries in both shells.
Albums, Artists, and Genres apply their matching built-in presentation to All Tracks; Playlists contains the shared List-tree projection.
The built-in entries remain read-only, while an All Tracks or saved-List context menu can create a root or derived List; saved Lists also expose Edit and either single-node or subtree Delete according to their descendants.
The native editor displays inherited and effective expressions, direct-membership capability, a live result preview, and the shared presentation catalog.
It validates the complete draft before enabling Create or Save and reports a failed mutation without changing the native tree optimistically.
After a successful save, it navigates to the authored List and applies the newly saved explicit presentation or the resolved automatic recommendation even when that List was already active.
Deletion first obtains the shared impact preview, lists every node in a cascade, and leaves removal of a directly writable tag from affected tracks unchecked by default.
The tree rebuilds only from committed library reset, List-upsert, and List-deletion publications, suppresses selection while replacing native nodes, retains surviving expansion, and expands the ancestors of the active List.
Deleting the active List or subtree falls back to All Tracks; delayed workspace observations tolerate the removed prior view.
Modern exposes the native navigation back affordance, and both shells route
`Alt+Left`, `Alt+Right`, and mouse back/forward buttons through the shared
workspace history. Availability is reread from that history after workspace
changes; the frontend owns no parallel stack.
While any List-authoring, library-transfer, or track-Properties modal workflow is active, the window absorbs those fixed history gestures without moving the workspace and does not admit another modal workflow.

The track list supports extended selection, keyboard navigation supplied by
`ListView`, double-click playback, native recycling, and sortable headers backed
by sortable runtime fields. Double-click playback targets the row under the
gesture regardless of any existing multi-row selection.
Headers and row cells are generated from the active
presentation; width, order, and visibility are shared per-list state. A header
edge resizes its column, a header context menu moves it, and the Columns menu
toggles fields while retaining at least one visible field.
The active primary sort is marked in its generated header. Clicking that header
reverses the persisted workspace direction; clicking another sortable header
starts that field in ascending order.
The shared width solver uses the current viewport, and a single horizontal
surface keeps headers and rows aligned when minimum widths overflow.
Viewport, column, and projection refreshes restore the selected rows and the prior top-visible track after replacing the native item source; an explicit reveal request takes precedence over that scroll anchor.
Presentation grouping inserts non-playable group headers through a display-index adapter while retaining projection row indices as the playback authority.
The presentation menu resolves built-in and restored custom presets through the shared catalog and picker policy.
It saves a per-List preference only after the complete selected specification is accepted by Runtime.
The existing `Ctrl+L` keymap action and native Now Playing/menu command submit a playback reveal request; WinUI follows its preferred view/List hints, selects and scrolls the visible row, and does nothing when the track remains hidden by the active projection or was removed.

Quick Filter places **Create List from current filter** beside the native suggestion box only when the shared filter state has a non-empty valid resolved expression.
The action opens the ordinary List editor beneath the active saved List, or as a root beneath a virtual source, and seeds the local expression with that resolved value rather than unresolved Quick text.

Properties is available for a non-empty track selection from the row context menu, the Modern overflow menu, the Classic View menu, and the fixed window-local `Alt+Enter` accelerator.
Right-clicking an unselected track selects it before presenting the row menu; group headings do not expose track authoring.
Opening Properties captures the current stable track ids and begins one revision-bound `TrackAuthoringSession`.
The native dialog projects the shared compact form: built-in metadata and common custom values are editable, mixed values are identified without becoming replacement text, tags common to all targets can be added or removed, new custom keys can be added, and technical audio properties remain read-only.
Metadata, tag, and custom-key suggestions use the active runtime completion vocabulary.
Save is disabled for an unchanged or invalid draft, prevents the dialog's default synchronous close while work is pending, and closes only after the combined metadata/tag Properties submission is accepted.
`Busy` and recoverable failures keep the draft open with an actionable message; stale or unavailable bindings disable submission and require reopening from the current selection.
Window retirement closes the dialog and suppresses late completion before releasing its runtime and selection owners.

The selected-row context menu lists writable tag-backed Playlists by stable List id, adds the captured selection through `ListMembershipAuthoringSession`, and offers explicit removal when the active List itself is directly writable.
It never infers editability from a List name or presentation.
The same menu exposes Manual Order Move Up, Move Down, Move to Top, Move to Bottom, and Reset Order according to `ListOrderAuthoringSession` capability flags.
The four movement handlers also receive the shared `Alt+Up`, `Alt+Down`, `Alt+Home`, and `Alt+End` accelerators; they remain native component actions rather than additions to the layout-document action schema.
WinUI does not expose drag reordering or Forget Hidden Positions in this version.

The native item view reports the complete display count while materializing rows
on demand; its row-model least-recently-used cache holds at most 2048 entries.
Cover art is asynchronous and ignores results from superseded selections.
Both shells pin the selection's artwork above the inspector's fields, where it stays put as those fields scroll. Classic caps it smaller than Modern does; neither cap changes what is shown.
One active-runtime loader coalesces group-heading, Inspector, Now Playing, and SMTC requests and holds at most 128 encoded-byte cache entries.
An invalid cover resource id displays `monogram` for a realized group heading, `vinyl` in Inspector, and `equalizer` in Now Playing.
Now Playing continues to display `equalizer` before playback starts and after transport returns to idle.
Windows exposes no placeholder preference in this version.
A valid resource id hides its placeholder while loading; absence or decode failure remains empty rather than being presented as confirmed no-cover.
When no group-heading or Inspector entity exists, both placeholder and decoded cover are hidden.

The Classic playback strip is ordered Soul, Play/Pause, Stop, Seek, Time, Volume. Clicking Classic Soul opens output selection, right-clicking opens the system menu, holding opens a full-screen Soul surface, and hovering describes the audio pipeline.

Modern Soul answers the same right-click, hold, and hover, and its click plays or pauses because Modern offers no separate Play/Pause button. Hovering either Soul describes the audio pipeline.

Selecting an output row in either shell submits its backend, device, and profile
ids through the shared UIModel selector and updates that exact requested tuple
in memory. The selector performs no synchronous settings write; the next
ordinary settings checkpoint persists the preference without replacing it with
the engine-confirmed Runtime snapshot. Presentation rows and operating-system
device names are not persisted.

SMTC commands route through the shared playback command surface. Playback observations update transport state and asynchronously replace system title, artist, album, and artwork metadata.

## Failure and cancellation

Folder-picker cancellation makes no change.
Before terminal playback retirement succeeds, a preparation failure leaves the
old process as the active retry target. Once preparation succeeds and release
begins, the old process is not a rollback target.
The restart coordinator attempts the successor launch after an explicit release operation either completes or throws an ordinary exception: the parent is exiting either way, so a half-released dying parent is not observable while a successor that never starts costs the user their session.
If the user closes the old window after restart is queued, the already-retired window/session pair counts as completed quiescence; the queued coordinator still launches the successor and exits the parent.
An exception that violates a destructor or other no-throw teardown boundary is instead an invariant fault and enters the process terminate boundary before successor launch; it is not a recoverable release result.
Native process-creation failure is reported by the already-retired parent,
which then exits. The shared launcher does not inherit unrelated handles and
preserves UTF-8 arguments across native quoting, including whitespace, quotes,
Unicode, and trailing backslashes.
Target validation, open, and window-activation failures are reported by the successor, which exits without changing the prior durable root.
A later ordinary launch may therefore reopen that prior root.
After successor activation, an initial-scan failure produces a visible diagnostic but retains the new active root; explicit-rescan planning or application failure likewise leaves the active session usable and retryable.
Session teardown requests scan cancellation, and its lifetime guard suppresses later presentation.
List editor, deletion-preview, membership, and order continuations use window or dialog lifetime guards.
The window cross-queries all three modal workflow owners before opening a dialog or moving workspace history, so a `ContentDialog` never competes with another dialog on the same `XamlRoot` and captured editor targets cannot move underneath an open modal surface.
Closing a dialog cancels its current work and terminalizes that dialog scope; opening a later dialog allocates a new scope rather than attempting to reuse the cancelled one.
Recoverable List validation, maintenance, stale-binding, and storage failures remain visible without publishing an optimistic tree or retargeting the captured selection.
Canceling a library-transfer mode dialog or Windows picker changes nothing, and rejecting a restore preview drops its one-shot plan.
Window retirement cancels the active picker and transfer lifetime, hides a native confirmation when possible, and suppresses every late completion before releasing Runtime borrowers.
Malformed YAML and recoverable import/export or picker failures remain visible through the shared notification history and status surface without changing the selected library optimistically.

WinUI lifecycle cleanup calls window close, timer stop, routed-handler removal, popup or flyout retirement, and property detachment directly on the owning dispatcher thread.
The frontend does not catch those calls merely because teardown is in progress: a failed HRESULT after the owners have met their thread and lifetime contracts is an invariant or native-runtime fault, and continuing through a partially retired graph is not a supported recovery path.

An optional WinRT operation may degrade only when its fallback is already complete and the failed operation leaves no callback or lifetime obligation behind.
That boundary catches `winrt::hresult_error`, records its message and HRESULT in the application log, and preserves the fallback; allocation failure and non-WinRT C++ exceptions continue to the ordinary exception boundary.
Mica uses the solid theme as its fallback, while final SMTC metadata disablement may be abandoned after command admission, subscriptions, and artwork work have already been retired.
If application logging itself is unavailable, the diagnostic boundary writes a debugger fallback; terminal application exit separately falls back to posting the native quit message.

Theme reload parses and validates a complete candidate before applying resources. A missing theme file uses built-in/system values. Any other read, syntax, token, type, or color failure keeps the last valid theme and displays the exact diagnostic.

Late cover-art results, late runtime events, and callbacks delivered after shell teardown are suppressed by generation, cancellation, or subscription lifetime.

## Persistence and versioning

Window placement, shell mode, library root, and pane sizes belong to the versioned Windows `desktop` group. Inline navigation and inspector boundaries are draggable, save their widths after a completed drag, and reuse those widths in both shells.
Window placement saves the native restored rectangle independently from the
maximized state, so saving while maximized does not replace the normal bounds.
Per-list presentation choice and column state use the shared `trackView.presentations` and `trackView.columnLayouts` groups.
Track-property and List-editor drafts and dialog state are not persisted; accepted metadata, custom metadata, tags, List definitions, and saved order use their ordinary library mutation contracts.
The editor's accepted presentation choice uses the shared per-List presentation-preference store rather than becoming a List field.
The three groups are saved in one atomic settings candidate independently from `windows-theme.yaml`.
Workspace owns the active view's current presentation and sorting.
`LibrarySession` restores that per-library workspace before controllers bind and checkpoints it with Windows settings changes and at shutdown.
The parent checkpoints the retiring workspace before launching a successor but never stores the requested root.
The successor attempts that root only after its window/session and process
adapters are active. It persists a copied settings candidate and replaces the
live snapshot only after the atomic save succeeds. Save failure does not roll
back the usable successor, but later ordinary settings checkpoints retain the
previous root rather than retrying the failed target. The process-global
`windows-playback.yaml` store is injected into `AppRuntime`: ordinary startup
restores it, while successor root durability gates observation and terminal
parent preparation removes its payload.
Exact paths, fields, defaults, validation, and versioning are defined by the [Windows desktop state reference](../../reference/windows/desktop-state.md).

## Frontend observations

Modern uses an integrated title bar, navigation, primary table, optional inspector, and persistent Now Playing surface. Classic uses a system title bar and dense desktop chrome. Neither visual treatment changes the runtime meaning of playback, output, selection, quality, or Soul aura.
WinUI packages the shared `note`, `vinyl`, and `equalizer` SVG sources plus the Soul brand mark and its license, even though the fixed Windows slot mapping currently selects only `monogram`, `vinyl`, and `equalizer`.
UIModel supplies style, monogram, and deterministic monogram foreground-color values; WinUI owns transparent XAML foreground rendering, responsive vinyl geometry, its current-theme-accent outer ring and one-third-diameter muted center label, and asset decoding.

## Implementation map

- [`app/windows-winui/CMakeLists.txt`](../../../app/windows-winui/CMakeLists.txt) owns the `aobus-winui-lib` static-library and thin `aobus-winui` executable boundary.
- [`App`](../../../app/windows-winui/App.xaml.h) owns the dispatcher, queued restart state, and [`LibraryWindowSession`](../../../app/windows-winui/app/LibraryWindowSession.h).
- [`LibraryWindowSession`](../../../app/windows-winui/app/LibraryWindowSession.cpp) owns one immutable window/session relationship and window-before-session release; [`LibrarySession`](../../../app/windows-winui/app/LibrarySession.h) owns one runtime, playback restore/admission, transactional selected-root commit, and the active-session scan workflow using shared [`runLibraryScan`](../../../app/include/ao/uimodel/library/task/LibraryScanOutcome.h).
- [`ao_desktop_launch`](../../../app/desktop/) owns common root, startup, protocol,
  and detached-launch rules. [`ProcessLauncher`](../../../app/windows-winui/platform/ProcessLauncher.cpp)
  owns real Win32 argument extraction and exact-executable discovery before
  delegating process creation.
- [`MainWindow`](../../../app/windows-winui/MainWindow.xaml) defines the window frame, its resources, its modal-owner admission rule, and the single region a shell is built into; [`ShellState`](../../../app/windows-winui/include/ao/winui/layout/ShellState.h) resolves its Windows-only responsive state, and the two shipped preset documents under [`app/windows-winui/layout/`](../../../app/windows-winui/layout/) define both native shells.
- [`MainWindowShell.cpp`](../../../app/windows-winui/shell/MainWindowShell.cpp), [`MainWindowTrack.cpp`](../../../app/windows-winui/track/MainWindowTrack.cpp), and [`MainWindowPlayback.cpp`](../../../app/windows-winui/playback/MainWindowPlayback.cpp) partition code-behind behavior by owner; XAML and generated code-behind declarations remain at the target root because WinUI generated-file association requires them.
- [`TrackListController`](../../../app/windows-winui/track/TrackListController.h), [`TrackItemView`](../../../app/windows-winui/track/TrackItemView.h), [`TrackDisplayIndex`](../../../app/include/ao/uimodel/library/track/TrackDisplayIndex.h), and [`IndexedTrackRowCache`](../../../app/include/ao/uimodel/library/track/IndexedTrackRowCache.h) own the grouped lazy table, selection reveal, and display/source index boundary.
- [`NavigationPane`](../../../app/windows-winui/layout/component/shell/NavigationPane.cpp) adapts the shared navigation tree and workspace history availability; [`MainWindow.xaml.cpp`](../../../app/windows-winui/MainWindow.xaml.cpp) adapts playback reveal events to the window's own track list.
- [`TrackPropertiesCoordinator`](../../../app/windows-winui/track/TrackPropertiesCoordinator.h) owns the native properties workflow, while [`TrackPropertiesAdapter`](../../../app/windows-winui/include/ao/winui/track/TrackPropertiesAdapter.h) keeps shared form and vocabulary mapping WinRT-free.
- [`TrackTable`](../../../app/windows-winui/layout/component/track/TrackTable.cpp), [`ShellBuilder`](../../../app/windows-winui/layout/ShellBuilder.cpp), and [`MainWindowTrack.cpp`](../../../app/windows-winui/track/MainWindowTrack.cpp) own the row menu, ordinary menu/action route, captured selection, and window-local dialog lifetime.
- [`ListAuthoringCoordinator`](../../../app/windows-winui/list/ListAuthoringCoordinator.h) owns native List CRUD, deletion preview, membership, and saved-order workflows; [`ListAuthoringAdapter`](../../../app/windows-winui/include/ao/winui/list/ListAuthoringAdapter.h) keeps committed tree invalidation and restoration policy free of WinRT types.
- [`LibraryTransferCoordinator`](../../../app/windows-winui/library/LibraryTransferCoordinator.h) owns native YAML mode dialogs, Windows pickers, restore confirmation, and transfer lifetime; [`LibraryTransferAdapter`](../../../app/windows-winui/include/ao/winui/library/LibraryTransferAdapter.h) maps selector rows and reports without WinRT types.
- [`TrackQuickFilterControl`](../../../app/windows-winui/track/TrackQuickFilterControl.h) and the `track.quickFilter` component in [`TrackComponents.cpp`](../../../app/windows-winui/layout/component/track/TrackComponents.cpp) own native completion plus valid-expression List creation.
- [`CoverArtPlaceholder`](../../../app/include/ao/uimodel/presentation/CoverArtPlaceholder.h), [`ResourceByteMemoryCache`](../../../app/include/ao/rt/resource/ResourceByteMemoryCache.h), and [`CoverArtPresenter`](../../../app/windows-winui/image/CoverArtPresenter.h) own shared placeholder policy, runtime byte delivery, and WinUI presentation respectively.
- [`AobusSoulControl`](../../../app/windows-winui/playback/AobusSoulControl.h) adapts the shared [`AobusSoulViewModel`](../../../app/include/ao/uimodel/playback/soul/AobusSoulViewModel.h).
- [`OutputDeviceControl`](../../../app/windows-winui/playback/OutputDeviceControl.h)
  adapts shared [`OutputDeviceViewModel`](../../../app/include/ao/uimodel/playback/output/OutputDeviceViewModel.h)
  rows; [`OutputSelection`](../../../app/include/ao/uimodel/playback/output/OutputSelection.h)
  owns pure restore admission and fallback resolution;
  [`DesktopOutputSelection`](../../../app/windows-winui/include/ao/winui/app/DesktopOutputSelection.h)
  adapts that rule to Windows settings, while `LibrarySession` submits the
  resolved runtime command and checkpoints requested intent.
- [`SmtcBridge`](../../../app/windows-winui/platform/SmtcBridge.h) and [`ThemeCoordinator`](../../../app/windows-winui/theme/ThemeCoordinator.h) own Windows media and theme adapters.
- [`StringResources`](../../../app/windows-winui/platform/StringResources.h) resolves dynamic authored copy from the same PRI resource system used by XAML `x:Uid`.
- [`WinUiErrorBoundary`](../../../app/windows-winui/include/ao/winui/WinUiErrorBoundary.h) owns optional-WinRT degradation and terminal diagnostic fallbacks; ordinary UI teardown does not use it.

## Test map

- [`ShellStateTest.cpp`](../../../test/unit/winui/layout/ShellStateTest.cpp) protects Windows breakpoints and shell-mode behavior.
- [`TrackDisplayIndexTest.cpp`](../../../test/unit/uimodel/library/track/TrackDisplayIndexTest.cpp) and [`IndexedTrackRowCacheTest.cpp`](../../../test/unit/uimodel/library/track/IndexedTrackRowCacheTest.cpp) protect grouping, source/display index mapping, and lazy row caching; runtime resource-byte tests protect shared cover delivery and stale-flight fencing.
- [`LibraryScanWorkflowTest.cpp`](../../../test/unit/uimodel/library/task/LibraryScanWorkflowTest.cpp) protects the scan decision shared by GTK and WinUI.
- [`AobusSoulViewModelTest.cpp`](../../../test/unit/uimodel/playback/soul/AobusSoulViewModelTest.cpp) protects shared geometry, colors, aura, periods, and frame gating.
- [`OutputSelectionTest.cpp`](../../../test/unit/uimodel/playback/output/OutputSelectionTest.cpp)
  protects catalog-aware persisted-route admission and fallback resolution.
- [`DesktopOutputSelectionTest.cpp`](../../../test/unit/winui/app/DesktopOutputSelectionTest.cpp)
  protects Windows startup resolution and the deferred-checkpoint preference update.
- [`DesktopSettingsYamlSchemaTest.cpp`](../../../test/unit/winui/DesktopSettingsYamlSchemaTest.cpp) and [`ThemeTest.cpp`](../../../test/unit/winui/ThemeTest.cpp) protect strict persistence and fallback.
- [`TrackPropertiesAdapterTest.cpp`](../../../test/unit/winui/track/TrackPropertiesAdapterTest.cpp) protects control-kind projection, mixed-state preservation, edit parsing, command eligibility, commit outcomes, and vocabulary suggestions on every host.
- [`ListAuthoringAdapterTest.cpp`](../../../test/unit/winui/list/ListAuthoringAdapterTest.cpp) protects post-save presentation resolution, committed tree invalidation, expansion restoration, active-ancestor reveal, and deterministic fallback; shared List editor, membership, and order-session tests protect the semantic workflows.
- [`LibraryTransferAdapterTest.cpp`](../../../test/unit/winui/library/LibraryTransferAdapterTest.cpp) protects all export/import selector mappings, restore-only confirmation, and report-complete native preview text; shared task-service and YAML-transfer tests protect execution and data semantics.
- [`KeymapAcceleratorPlanTest.cpp`](../../../test/unit/winui/input/KeymapAcceleratorPlanTest.cpp) protects installation of the native-only saved-order handlers without expanding the layout-action schema.
- Tests under [`test/unit/desktop/`](../../../test/unit/desktop/) protect shared
  successor arguments, strict root planning, same-root identity, detached
  launch, native quoting, and handle inheritance on both hosts.
- [`DestructiveLibraryRestartTest.cpp`](../../../test/unit/winui/app/DestructiveLibraryRestartTest.cpp)
  and [`SelectedRootCommitTest.cpp`](../../../test/unit/winui/app/SelectedRootCommitTest.cpp)
  protect preparation failure, release-before-launch ordering, and fail-closed
  root candidate mutation.
- [`WinUiErrorBoundaryTest.cpp`](../../../test/unit/winui/WinUiErrorBoundaryTest.cpp) proves that the optional boundary contains WinRT HRESULT failures without hiding ordinary C++ exceptions.
- WinUI-owned tests under [`test/unit/winui/`](../../../test/unit/winui/) that
  need a native host are compiled only by the native Windows profile. Windows
  shell policy carrying no WinRT dependency - settings compatibility,
  output-preference resolution, root-commit sequencing, the component schema,
  and the keyboard-accelerator plan - compiles into `ao_core_test` on every
  host, as do the shared desktop rule tests.
- Native Debug and Release `winui` builds protect `aobus-winui-lib`, generated C++/WinRT, XAML, process launch, PRI resources, WASAPI, picker, and SMTC integration.

## Related documents

- [Interactive session lifecycle architecture](../../architecture/interactive-session-lifecycle.md)
- [Desktop library lifecycle specification](../application/desktop-library-lifecycle.md)
- [Desktop successor protocol reference](../../reference/application/desktop-successor-protocol.md)
- [Playback session persistence specification](../playback/session-persistence.md)
- [Presentation architecture](../../architecture/presentation.md)
- [Windows desktop state reference](../../reference/windows/desktop-state.md)
- [Use the Windows desktop](../../user/use-windows-desktop.md)
