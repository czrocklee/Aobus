---
id: shell.windows-desktop
type: spec
status: current
domain: application-shell
summary: Defines the native Windows desktop shell, mode switching, library replacement, responsive layout, and Windows integration behavior.
---
# Windows desktop shell

## Scope

This specification owns the observable behavior of the native WinUI 3 desktop frontend. It covers its Modern and Classic shells, shared session lifetime, library selection and rescan, track-list interaction, Soul behavior, theme reload, and Windows media integration.

It does not define library scan semantics, playback engine behavior, or the exact Windows YAML fields. Those remain with their subsystem specifications and the [Windows desktop state reference](../../reference/windows/desktop-state.md).

## Code boundary

The [system architecture](../../architecture/system-overview.md) defines the runtime-to-UIModel-to-frontend dependency direction. The [application shell architecture](../../architecture/application-shell.md) owns shell composition. Shared policy lives under `app/include/ao/uimodel/`; native controls and Windows adapters live under `app/windows-winui/`.

## Terminology

- **Library session**: the window-independent owner of the active library runtime, playback runtime, settings, and replacement workflow.
- **Modern**: the default integrated-title-bar shell with navigation, track list, inspector, and persistent Now Playing area.
- **Classic**: the system-title-bar, high-density shell with menus, toolbar, library tree, property panel, status bar, and GTK-compatible playback-strip order.
- **Candidate runtime**: a fully separate runtime prepared before it may replace the active library runtime.

## Invariants

- One `LibrarySession` and one main window remain alive while either shell is selected.
- Switching shells changes presentation only; it neither scans the library nor interrupts playback.
- All runtime and UIModel callbacks that touch XAML state execute through the window dispatcher.
- The window detaches its observers and controllers before their runtime is destroyed or replaced.
- Modern is the first-run shell. Both shells follow the effective Windows light or dark theme unless semantic theme tokens override their surfaces.
- Authored Windows shell copy, dynamic status templates, presentation labels, and column labels resolve through the WinUI resource system. Library metadata, device names, paths, and diagnostics supplied by lower layers remain data rather than resource identifiers.
- The track table remains usable at every supported width. The inspector collapses before navigation; narrow layouts also collapse navigation.
- Soul radius, anchor geometry, brand colors, aura interpretation, gradient stops, and animation periods come from shared UIModel constants. Theme YAML cannot replace them.
- A shell presentation may apply a positive stroke width and inner-glyph scale to fit its allocated control size without changing those shared brand and motion constants.

## State model

The shell mode is `modern` or `classic`.
Responsive state is derived from the current client width.
Below 720 effective pixels navigation uses the minimal overlay presentation and the inspector is available as an explicit overlay.
From 720 through 1119 navigation uses the compact presentation and the inspector remains an explicit overlay.
At 1120 or wider navigation is expanded and the inspector is inline.
Below the wide tier the browser summary yields its space to the filter. At the
narrow tier the Now Playing artwork and text yield their space to transport,
time, volume, and overflow commands.

The active library runtime and playback runtime normally refer to the same runtime. During a successful library replacement, current playback may retain the retiring runtime until transport becomes idle, then playback adopts the new library runtime.

Soul frame updates run only while the control is loaded, its app window is
visible and not minimized, and playback requires animation. Window focus alone
does not pause an otherwise visible animation.
Modern applies the compact control-local values `strokeWidth: 5.0` and `glyphScale: 0.85`, and renders the Play/Pause glyph independently of the rotating ring so it remains upright.
Classic renders a 32 by 32 glyph-free Soul with the shared default stroke width independently from its ordinary Play/Pause and Stop buttons.

## Commands and transitions

Selecting Modern or Classic saves the selection and updates title-bar ownership before showing the selected shell. Existing list selection, library data, and playback continue.

Open Library uses the Windows folder picker initialized from the current `AppWindow` id.
When the selected root already contains the canonical database, a different root creates and loads a candidate runtime without an implicit scan, then swaps the library authority after successful preparation.
When the selected root has no canonical database, the candidate completes its initial scan before the swap.
Selecting the already active indexed root is a no-op.
Rescan runs the transactional scan workflow against the active runtime, then reloads its projections after success; it cannot open a second LMDB environment for the same database.
Cancel and pre-commit failure retain the active authority.
Superseding an operation invalidates its generation before requesting
cancellation, so a late completion cannot publish status or replace the active
runtime. Candidate roots remain registered until their runtime retires, which
prevents rapid repeated requests from opening one LMDB environment twice.

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
One library-runtime loader coalesces group-heading and Inspector requests and holds at most 128 encoded-byte cache entries.
Now Playing artwork uses the playback runtime, including while playback temporarily retains the retiring library runtime, and has an independent bounded cache.
An invalid cover resource id displays `monogram` for a realized group heading, `vinyl` in Inspector, and `equalizer` in Now Playing.
Now Playing continues to display `equalizer` before playback starts and after transport returns to idle.
Windows exposes no placeholder preference in this version.
A valid resource id hides its placeholder while loading; absence or decode failure remains empty rather than being presented as confirmed no-cover.
When no group-heading or Inspector entity exists, both placeholder and decoded cover are hidden.

The Classic playback strip is ordered Soul, Play/Pause, Stop, Seek, Time, Volume. Clicking Classic Soul opens output selection, right-clicking opens the system menu, holding opens a full-screen Soul surface, and hovering describes the audio pipeline.

SMTC commands route through the shared playback command surface. Playback observations update transport state and asynchronously replace system title, artist, album, and artwork metadata.

## Failure and cancellation

Folder-picker cancellation makes no change.
Cancelling an initial scan destroys its candidate and reports cancellation without mutating the active session.
Candidate creation or loading failures, and initial-scan or explicit-rescan planning, application, and reload failures, retain the active library and produce a visible diagnostic.

Theme reload parses and validates a complete candidate before applying resources. A missing theme file uses built-in/system values. Any other read, syntax, token, type, or color failure keeps the last valid theme and displays the exact diagnostic.

Late cover-art results, late runtime events, and callbacks delivered after shell teardown are suppressed by generation, cancellation, or subscription lifetime.

## Persistence and versioning

Window placement, shell mode, library root, and pane sizes belong to the versioned Windows `desktop` group. Inline navigation and inspector boundaries are draggable, save their widths after a completed drag, and reuse those widths in both shells.
Window placement saves the native restored rectangle independently from the
maximized state, so saving while maximized does not replace the normal bounds.
Per-list presentation choice and column state use the shared `trackView.presentations` and `trackView.columnLayouts` groups.
The three groups are saved in one atomic settings candidate independently from `windows-theme.yaml`.
Workspace owns the active view's current presentation and sorting.
`LibrarySession` restores that per-library workspace before controllers bind,
checkpoints it with Windows settings changes and at shutdown, and checkpoints
the retiring workspace before a successful root replacement.
Exact paths, fields, defaults, validation, and versioning are defined by the [Windows desktop state reference](../../reference/windows/desktop-state.md).

## Frontend observations

Modern uses an integrated title bar, navigation, primary table, optional inspector, and persistent Now Playing surface. Classic uses a system title bar and dense desktop chrome. Neither visual treatment changes the runtime meaning of playback, output, selection, quality, or Soul aura.
WinUI packages the shared `note`, `vinyl`, and `equalizer` SVG sources plus the Soul brand mark and its license, even though the fixed Windows slot mapping currently selects only `monogram`, `vinyl`, and `equalizer`.
UIModel supplies style, monogram, and deterministic monogram foreground-color values; WinUI owns transparent XAML foreground rendering, responsive vinyl geometry, its current-theme-accent outer ring and one-third-diameter muted center label, and asset decoding.

## Implementation map

- [`App`](../../../app/windows-winui/App.xaml.h) owns dispatcher, session, and window lifetime.
- [`LibrarySession`](../../../app/windows-winui/app/LibrarySession.h) owns prepare-then-swap and playback-runtime retention, using the shared [`LibraryScanWorkflow`](../../../app/include/ao/uimodel/library/task/LibraryScanWorkflow.h).
- [`MainWindow`](../../../app/windows-winui/MainWindow.xaml) defines both native shells.
- [`MainWindowShell.cpp`](../../../app/windows-winui/shell/MainWindowShell.cpp), [`MainWindowTrack.cpp`](../../../app/windows-winui/track/MainWindowTrack.cpp), and [`MainWindowPlayback.cpp`](../../../app/windows-winui/playback/MainWindowPlayback.cpp) partition code-behind behavior by owner; XAML and generated code-behind declarations remain at the target root because WinUI generated-file association requires them.
- [`TrackListController`](../../../app/windows-winui/track/TrackListController.h), [`TrackItemView`](../../../app/windows-winui/track/TrackItemView.h), [`TrackDisplayIndex`](../../../app/include/ao/uimodel/library/track/TrackDisplayIndex.h), and [`IndexedTrackRowCache`](../../../app/include/ao/uimodel/library/track/IndexedTrackRowCache.h) own the grouped lazy table.
- [`CoverArtPlaceholder`](../../../app/include/ao/uimodel/presentation/CoverArtPlaceholder.h), [`WindowsCoverArtLoader`](../../../app/windows-winui/image/WindowsCoverArtLoader.h), and [`CoverArtPresenter`](../../../app/windows-winui/image/CoverArtPresenter.h) own shared placeholder policy and WinUI delivery.
- [`AobusSoulControl`](../../../app/windows-winui/playback/AobusSoulControl.h) adapts the shared [`AobusSoulViewModel`](../../../app/include/ao/uimodel/playback/soul/AobusSoulViewModel.h).
- [`SmtcBridge`](../../../app/windows-winui/platform/SmtcBridge.h) and [`WindowsThemeCoordinator`](../../../app/windows-winui/theme/WindowsThemeCoordinator.h) own Windows media and theme adapters.
- [`WindowsStringResources`](../../../app/windows-winui/platform/WindowsStringResources.h) resolves dynamic authored copy from the same PRI resource system used by XAML `x:Uid`.

## Test map

- [`DesktopShellPolicyTest.cpp`](../../../test/unit/uimodel/layout/shell/DesktopShellPolicyTest.cpp) protects breakpoints and mode policy.
- [`TrackDisplayIndexTest.cpp`](../../../test/unit/uimodel/library/track/TrackDisplayIndexTest.cpp), [`IndexedTrackRowCacheTest.cpp`](../../../test/unit/uimodel/library/track/IndexedTrackRowCacheTest.cpp), and [`CoverArtRequestModelTest.cpp`](../../../test/unit/uimodel/library/track/CoverArtRequestModelTest.cpp) protect grouping, cache bounds, and stale suppression.
- [`LibraryScanWorkflowTest.cpp`](../../../test/unit/uimodel/library/task/LibraryScanWorkflowTest.cpp) protects the scan decision shared by GTK and WinUI.
- [`AobusSoulViewModelTest.cpp`](../../../test/unit/uimodel/playback/soul/AobusSoulViewModelTest.cpp) protects shared geometry, colors, aura, periods, and frame gating.
- [`WindowsDesktopSettingsYamlSchemaTest.cpp`](../../../test/unit/uimodel/layout/shell/WindowsDesktopSettingsYamlSchemaTest.cpp) and [`WindowsThemeTest.cpp`](../../../test/unit/uimodel/preference/WindowsThemeTest.cpp) protect strict persistence and fallback.
- Native Debug and Release `winui` builds protect C++/WinRT, XAML, PRI resources, WASAPI, picker, and SMTC integration.

## Related documents

- [Interactive session lifecycle architecture](../../architecture/interactive-session-lifecycle.md)
- [Presentation architecture](../../architecture/presentation.md)
- [Windows desktop state reference](../../reference/windows/desktop-state.md)
- [Use the Windows desktop](../../user/use-windows-desktop.md)
