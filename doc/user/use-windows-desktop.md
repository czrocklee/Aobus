---
id: user.windows-desktop
---
# Use the Windows desktop

## Outcome

You will open an indexed music folder in the native Windows application, browse, edit, play, back up, and restore library data, switch between Modern and Classic layouts without interrupting playback, and optionally reload a semantic theme.

## Prerequisites

- Windows 11 24H2 or Windows Server 2025 on x64.
- The unpackaged, framework-dependent Aobus WinUI build and its Windows App Runtime dependency.
- A folder containing supported music files on a filesystem local to this Windows host.

The Windows frontend keeps its database beneath the selected music root. Network shares, including mapped network drives and UNC shares, are not supported database locations: reaching the files does not establish safe database locking. Copy the library to local storage before opening it; the [database reference](../reference/library/storage/database.md#environment) explains the storage boundary.

## Steps

1. Start `Aobus.exe`. Modern mode opens by default and an empty first-run library is harmless.
2. Choose **Open Library...** and select the root of your music library.
   Selecting the current root does nothing.
   Opening a different root saves and closes the current Windows process, then
   starts a new Aobus process for the selected library; playback from the old
   library stops and the replacement has a new window identity.
   Aobus opens an existing index directly.
   For a folder without an Aobus index, the new process activates the folder and
   then runs the initial scan. If scanning fails, that folder stays open so
   **Rescan** can retry it.
3. Use **Folders**, **Albums**, **Artists**, **Genres**, or a list below
   **Playlists** in the navigation pane. Use the presentation button above the
   table to switch between songs, albums, artists, genres, years, classical
   views, restored custom presets, and the other available presentations. Grouped presentations add
   album or category headings without turning those headings into playable
   rows.
   Right-click **All Tracks** to create a root List, or right-click a saved List to create a derived child, edit its definition and preferred presentation, or preview and delete it.
   A List with descendants offers **Delete List and Descendants...** and shows the complete subtree before committing; deleting a List never deletes music files.
   Use the Modern back button, `Alt+Left`/`Alt+Right`, or the mouse back/forward buttons to traverse workspace history.
4. Type in **Filter tracks** to narrow the active view. Select one or more rows
   with ordinary Windows list gestures, use a sortable column header to change
   ordering, and double-click a track row to start playback. Drag a header's
   right edge to resize it, right-click a header to move it, and use **More >
   Columns** in Modern or **View > Columns** in Classic to show or hide fields.
   See [filtering and suggestions](manage-library.md#understand-filtering-and-suggestions) for Quick versus Expression mode and why romanized suggestions must be accepted to search their original text.
   When the current filter is non-empty and valid, choose **Create List from current filter** to open the List editor with the resolved expression already filled in.
   Right-click a selected row and choose **Properties...**, choose the same command from **More** or Classic's **View** menu, or press `Alt+Enter` to edit built-in metadata, tags, and custom metadata for the captured selection.
   Technical audio properties remain read-only, and Save becomes available only after a valid change.
   The same row menu can add tracks to eligible Playlists or explicitly remove them from the active tag-backed Playlist.
   On a saved List using a flat unsorted presentation, its **Manual Order** submenu moves the selection up, down, to the top, or to the bottom and can reset saved positions; the movement shortcuts are `Alt+Up`, `Alt+Down`, `Alt+Home`, and `Alt+End`.
5. Inspect the selected track in the details pane. Drag either inline pane
   boundary to resize navigation or details; Aobus remembers both widths. On
   medium or narrow windows, use the details button to open it as an overlay;
   the navigation pane becomes compact or minimal automatically.
   Missing artwork uses a monogram in group headings and a transparent vinyl placeholder in the details pane; these Windows choices are fixed in this version.
6. Use the persistent Now Playing controls for previous, play/pause, next, shuffle, repeat, seeking, volume, and output.
   Choose **Reveal Current Track** or press `Ctrl+L` to select and scroll the playing track into view when it is visible in its target presentation.
   Aobus remembers the output backend, device, and profile you request and tries
   to restore them after providers are discovered on the next launch. A named
   device may remain preferred while disconnected; until it can be selected,
   the runtime-selected default remains active.
   Missing Now Playing artwork and idle playback use a transparent equalizer placeholder.
7. Choose **Classic Mode** to use the dense menu, tree, property, status, and GTK-compatible playback layout. Choose **Modern Mode** to return. Playback, the active library, list, and presentation continue across the switch.
8. In Modern mode, click Soul to play or pause. In Classic mode, click Soul to choose an output device.
   In either mode, right-click Soul for the system menu, hold it for full-screen Soul, or hover it to inspect the audio pipeline. See [interpreting playback quality](play-music.md#inspect-playback-quality) to distinguish source fidelity, pipeline changes, and incomplete evidence.
9. Choose **Rescan** after files change. A failed scan leaves the active library open and retryable. Another Rescan while one is active starts nothing; choosing a different library instead closes the current process and cancels its work during teardown.
10. To back up portable library data, choose **More → Export Library Data...** in Modern mode or **File → Export Library Data...** in Classic mode, select Delta, Metadata, Full, or List Only, and save a `.yaml` file.
    To import one, use the matching **Import Library Data...** command and choose Merge to retain records outside the backup or Restore to replace the reported scope.
    Restore shows the payload version, mode, scope, change counts, and ignored references before enabling the scope-specific destructive action; Cancel remains the default.
    Modern shows transfer progress in its ordinary activity surface. Switching to Classic leaves an admitted transfer running and reports its terminal status there; closing its dialog, picker, or the window cancels the unfinished workflow.
11. To apply a custom theme, create `%LOCALAPPDATA%\Aobus\windows-theme.yaml` using the exact reference schema, then choose **Reload Theme**. Omit or remove the file to follow built-in and system appearance.

## Verify the result

The status area reports the selected library as ready, rows remain keyboard navigable, YAML transfers appear in Modern's activity surface, and Windows media keys and the system media overlay reflect the active track. Switching modes must not restart a scan or transfer or stop audio.

## Troubleshooting

- If a selected folder cannot be opened, the successor process reports startup failure and exits; start Aobus normally to return to the previously saved library. A scan failure instead leaves the selected library open and retryable.
- If the root is on a network share, switching between a mapped drive letter and UNC spelling does not make it a supported database location. Use local storage; a remote desktop connection to Windows does not itself make that host's local disk a network filesystem.
- If theme reload fails, correct the reported field, type, or color. The last valid theme remains visible. Removing the file and choosing **Reload Theme** restores the Windows appearance.
- If an import is rejected, review the notification detail for malformed YAML, an unsupported version, an unsafe path, or source/target evidence that changed after preview. The target remains unchanged when import fails before commit.
- If no audio device appears, confirm that Windows Audio is running and that the desired endpoint is enabled.
- If media keys do not work in a remote session, verify them once in an interactive console or RDP desktop; redirected keys may be consumed by the client.

## Related reference

- [Windows desktop state](../reference/windows/desktop-state.md)
- [Windows desktop shell specification](../system/frontend/windows.md)
- [Back up and restore library data](backup-and-restore.md)
- [Supported audio files](../reference/media/audio-file.md)
