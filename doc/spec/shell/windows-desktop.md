---
id: shell.windows-desktop
type: spec
status: current
domain: application-shell
summary: Defines the native Windows desktop shell, mode switching, destructive library restart, responsive layout, and Windows integration behavior.
---
# Windows desktop shell

## Scope

This specification owns the observable behavior of the native WinUI 3 desktop frontend. It covers its Modern and Classic shells, shared session lifetime, WinUI adaptation of library selection and rescan, track-list interaction, Soul behavior, theme reload, and Windows media integration.

It does not redefine the [shared desktop library lifecycle](../application/desktop-library-lifecycle.md), its [private successor arguments](../../reference/application/desktop-successor-protocol.md), library scan semantics, playback engine behavior, or the exact Windows YAML fields. Those remain with their shared owners and the [Windows desktop state reference](../../reference/windows/desktop-state.md).

## Code boundary

The [system architecture](../../architecture/system-overview.md) defines the runtime-to-UIModel-to-frontend dependency direction. The [application shell architecture](../../architecture/application-shell.md) owns shell composition. Cross-desktop root, startup, protocol, and detached-launch rules live in `ao_app_desktop`; shared presentation policy lives under `app/include/ao/uimodel/`. Windows-only lifecycle effects, policy, XAML, controls, and adapters live in the Windows-only `aobus-winui-lib` under `app/windows-winui/`, while `aobus-winui` is the thin final-link and deployed-resource target.

## Terminology

- **Library session**: the library-bound owner of one runtime, settings, scan workflow, and playback-command surface; one process-lifetime `LibraryWindowSession` owns it together with its `MainWindow`.
- **Modern**: the default integrated-title-bar shell with navigation, track list, inspector, and persistent Now Playing area.
- **Classic**: the system-title-bar, high-density shell with menus, toolbar, library tree, property panel with artwork, status bar, and GTK-compatible playback-strip order.

## Invariants

- One process owns at most one `LibrarySession`, one unique runtime, and one main window; opening a different root replaces the process rather than adding another graph.
- A different-root restart prepares the still-live graph by checkpointing state and terminally retiring playback persistence. Retirement failure leaves that graph usable and launches no successor.
- The successor does not restore playback or admit playback writes until its selected root is durable. A failed root commit preserves the prior live settings snapshot and permanently seals playback writes.
- Switching shells changes presentation only; it neither scans the library nor interrupts playback.
- All runtime and UIModel callbacks that touch XAML state execute through the window dispatcher.
- The window detaches its observers and controllers before its session destroys the unique runtime.
- Modern is the first-run shell. Both shells follow the effective Windows light or dark theme unless semantic theme tokens override their surfaces.
- Authored Windows shell copy, dynamic status templates, presentation labels, and column labels resolve through the WinUI resource system. Library metadata, device names, paths, and diagnostics supplied by lower layers remain data rather than resource identifiers.
- The track table remains usable at every supported width. The inspector collapses before navigation; narrow layouts also collapse navigation.
- Soul radius, anchor geometry, brand colors, aura interpretation, gradient stops, and animation periods come from shared UIModel constants. Theme YAML cannot replace them.
- A shell mode may apply a positive stroke width and inner-glyph scale to fit its allocated control size without changing those shared brand and motion constants.

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
There is no live-runtime exchange inside `LibrarySession`.
Open Library may cancel an active scan through parent teardown; an already queued process transition rejects another Open Library request, while an active Rescan still reports its current operation for another Rescan.

Navigation exposes read-only Folders, Albums, Artists, Genres, and Playlists
entries in both shells. Albums, Artists, and Genres apply their matching
built-in presentation to All Tracks; Playlists contains the shared list-tree
projection. This shell does not create, rename, move, or delete list nodes.

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
Presentation grouping inserts non-playable group headers through a display-index adapter while retaining projection row indices as the playback authority.
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

SMTC commands route through the shared playback command surface. Playback observations update transport state and asynchronously replace system title, artist, album, and artwork metadata.

## Failure and cancellation

Folder-picker cancellation makes no change.
Before terminal playback retirement succeeds, a preparation failure leaves the
old process as the active retry target. Once preparation succeeds and release
begins, the old process is not a rollback target.
The restart coordinator attempts the successor launch after an explicit release operation either completes or throws an ordinary exception: the parent is exiting either way, so a half-released dying parent is not observable while a successor that never starts costs the user their session.
An exception that violates a destructor or other no-throw teardown boundary is instead an invariant fault and enters the process terminate boundary before successor launch; it is not a recoverable release result.
Native process-creation failure is reported by the already-retired parent,
which then exits. The shared launcher does not inherit unrelated handles and
preserves UTF-8 arguments across native quoting, including whitespace, quotes,
Unicode, and trailing backslashes.
Target validation, open, and window-activation failures are reported by the successor, which exits without changing the prior durable root.
A later ordinary launch may therefore reopen that prior root.
After successor activation, an initial-scan failure produces a visible diagnostic but retains the new active root; explicit-rescan planning or application failure likewise leaves the active session usable and retryable.
Session teardown requests scan cancellation, and its lifetime guard suppresses later presentation.

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
- [`LibraryWindowSession`](../../../app/windows-winui/app/LibraryWindowSession.cpp) owns one immutable window/session relationship and window-before-session release; [`LibrarySession`](../../../app/windows-winui/app/LibrarySession.h) owns one runtime, playback restore/admission, transactional selected-root commit, and the active-session scan workflow using the shared [`LibraryScanWorkflow`](../../../app/include/ao/uimodel/library/task/LibraryScanWorkflow.h).
- [`ao_app_desktop`](../../../app/desktop/) owns common root, startup, protocol,
  and detached-launch rules. [`ProcessLauncher`](../../../app/windows-winui/platform/ProcessLauncher.cpp)
  owns real Win32 argument extraction and exact-executable discovery before
  delegating process creation.
- [`MainWindow`](../../../app/windows-winui/MainWindow.xaml) defines the window frame, its resources, and the single region a shell is built into; [`ShellStatePolicy`](../../../app/windows-winui/include/ao/winui/layout/ShellStatePolicy.h) resolves its Windows-only responsive state, and the two shipped preset documents under [`app/windows-winui/layout/`](../../../app/windows-winui/layout/) define both native shells.
- [`MainWindowShell.cpp`](../../../app/windows-winui/shell/MainWindowShell.cpp), [`MainWindowTrack.cpp`](../../../app/windows-winui/track/MainWindowTrack.cpp), and [`MainWindowPlayback.cpp`](../../../app/windows-winui/playback/MainWindowPlayback.cpp) partition code-behind behavior by owner; XAML and generated code-behind declarations remain at the target root because WinUI generated-file association requires them.
- [`TrackListController`](../../../app/windows-winui/track/TrackListController.h), [`TrackItemView`](../../../app/windows-winui/track/TrackItemView.h), [`TrackDisplayIndex`](../../../app/include/ao/uimodel/library/track/TrackDisplayIndex.h), and [`IndexedTrackRowCache`](../../../app/include/ao/uimodel/library/track/IndexedTrackRowCache.h) own the grouped lazy table.
- [`CoverArtPlaceholder`](../../../app/include/ao/uimodel/presentation/CoverArtPlaceholder.h), [`ResourceByteLoader`](../../../app/include/ao/rt/resource/ResourceByteLoader.h), and [`CoverArtPresenter`](../../../app/windows-winui/image/CoverArtPresenter.h) own shared placeholder policy, runtime byte delivery, and WinUI presentation respectively.
- [`AobusSoulControl`](../../../app/windows-winui/playback/AobusSoulControl.h) adapts the shared [`AobusSoulViewModel`](../../../app/include/ao/uimodel/playback/soul/AobusSoulViewModel.h).
- [`SmtcBridge`](../../../app/windows-winui/platform/SmtcBridge.h) and [`ThemeCoordinator`](../../../app/windows-winui/theme/ThemeCoordinator.h) own Windows media and theme adapters.
- [`StringResources`](../../../app/windows-winui/platform/StringResources.h) resolves dynamic authored copy from the same PRI resource system used by XAML `x:Uid`.
- [`WinUiErrorBoundary`](../../../app/windows-winui/include/ao/winui/WinUiErrorBoundary.h) owns optional-WinRT degradation and terminal diagnostic fallbacks; ordinary UI teardown does not use it.

## Test map

- [`ShellStatePolicyTest.cpp`](../../../test/unit/winui/layout/ShellStatePolicyTest.cpp) protects Windows breakpoints and shell-mode policy.
- [`TrackDisplayIndexTest.cpp`](../../../test/unit/uimodel/library/track/TrackDisplayIndexTest.cpp) and [`IndexedTrackRowCacheTest.cpp`](../../../test/unit/uimodel/library/track/IndexedTrackRowCacheTest.cpp) protect grouping and lazy row caching; runtime resource-byte tests protect shared cover delivery and stale-flight fencing.
- [`LibraryScanWorkflowTest.cpp`](../../../test/unit/uimodel/library/task/LibraryScanWorkflowTest.cpp) protects the scan decision shared by GTK and WinUI.
- [`AobusSoulViewModelTest.cpp`](../../../test/unit/uimodel/playback/soul/AobusSoulViewModelTest.cpp) protects shared geometry, colors, aura, periods, and frame gating.
- [`DesktopSettingsYamlSchemaTest.cpp`](../../../test/unit/winui/DesktopSettingsYamlSchemaTest.cpp) and [`ThemeTest.cpp`](../../../test/unit/winui/ThemeTest.cpp) protect strict persistence and fallback.
- Tests under [`test/unit/desktop/`](../../../test/unit/desktop/) protect shared
  successor arguments, strict root planning, same-root identity, detached
  launch, native quoting, and handle inheritance on both hosts.
- [`DestructiveLibraryRestartTest.cpp`](../../../test/unit/winui/app/DestructiveLibraryRestartTest.cpp)
  and [`SelectedRootCommitTest.cpp`](../../../test/unit/winui/app/SelectedRootCommitTest.cpp)
  protect preparation failure, release-before-launch ordering, and fail-closed
  root candidate mutation.
- [`WinUiErrorBoundaryTest.cpp`](../../../test/unit/winui/WinUiErrorBoundaryTest.cpp) proves that the optional boundary contains WinRT HRESULT failures without hiding ordinary C++ exceptions.
- WinUI-owned tests under [`test/unit/winui/`](../../../test/unit/winui/) are
  compiled only by the native Windows profile. The shared desktop rule tests
  compile into `ao_core_test` on Linux and Windows.
- Native Debug and Release `winui` builds protect `aobus-winui-lib`, generated C++/WinRT, XAML, process launch, PRI resources, WASAPI, picker, and SMTC integration.

## Related documents

- [Interactive session lifecycle architecture](../../architecture/interactive-session-lifecycle.md)
- [Desktop library lifecycle specification](../application/desktop-library-lifecycle.md)
- [Desktop successor protocol reference](../../reference/application/desktop-successor-protocol.md)
- [Playback session persistence specification](../playback/session-persistence.md)
- [Presentation architecture](../../architecture/presentation.md)
- [Windows desktop state reference](../../reference/windows/desktop-state.md)
- [Use the Windows desktop](../../user/use-windows-desktop.md)
