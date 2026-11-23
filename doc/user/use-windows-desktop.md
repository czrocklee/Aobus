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
   then runs the initial scan. If scanning fails, the folder stays open; choose
   **More → Rescan Library** in Modern or **File → Rescan** in Classic to retry.
3. Choose **All Tracks** or a saved List in the navigation pane. Expand a parent List to browse its children.
   A new view uses the List's preferred presentation, or the recommendation if no preference is available. Returning to an open view keeps its presentation.
   In Modern, use the presentation button above the table to choose **Songs**, **Albums**, **Artists**, **Genres**, **Years**, a classical view, or a restored custom preset. This choice becomes the List's preference. Group headings are not playable rows.
   Right-click **All Tracks** to create a root List, or right-click a saved List to create a derived child, edit its definition and preferred presentation, or preview and delete it.
   A List with descendants offers **Delete List and Descendants...** and shows the complete subtree before committing; deleting a List never deletes music files.
   Use the Modern back button, `Alt+Left`/`Alt+Right`, or the mouse back/forward buttons to revisit earlier views without changing the List's preference.
4. Type in **Filter tracks** to narrow the active view. Select one or more rows
   with ordinary Windows list gestures, use a sortable column header to change
   ordering, and double-click a track row to start playback. Drag a header's
   right edge to resize it, right-click a header to move it, and use **More >
   Columns** in Modern or **View > Columns** in Classic to show or hide fields.
   See [filtering and suggestions](manage-library.md#understand-filtering-and-suggestions) for Quick versus Expression mode and why romanized suggestions must be accepted to search their original text.
   When the current filter is non-empty and valid, choose **Create List from current filter** to open the List editor with the resolved expression already filled in.
   To edit built-in metadata, tags, and custom metadata, right-click a selected row and choose **Properties...**, choose the same command from **More** in Modern or **View** in Classic, or press `Alt+Enter`.
   **Properties** edits the tracks selected when it opens, even if the selection changes afterward.
   Use a category **Credits scope:** button to edit its full list of names and optional roles, not the `+N` preview. The category kind is fixed; choose **Credits scope: All credit kinds** to change kinds. If the tracks differ in that scope, choose **Replace this scope on all selected tracks** or **Clear this scope**; credits outside it stay as they are.
   Choose **Save credits** to keep the edit in **Properties** without writing it to the library, or **Cancel** to discard just that Credits edit. Then choose **Save** to apply pending metadata, Credits, and tags together. See [Edit Credits](edit-metadata.md#edit-credits).
   **Audio Properties** remain read-only, and **Save** becomes available only after a valid change.
   The same row menu can add tracks to eligible Playlists or explicitly remove them from the active tag-backed Playlist.
   On a saved List using a flat unsorted presentation, its **Manual Order** submenu moves the selection up, down, to the top, or to the bottom and can reset saved positions; the movement shortcuts are `Alt+Up`, `Alt+Down`, `Alt+Home`, and `Alt+End`.
5. Inspect the selected track in the details pane. Drag either inline pane
   boundary to resize navigation or details; Aobus remembers both widths. On
   medium or narrow windows, use the details button to open it as an overlay;
   the navigation pane becomes compact or minimal automatically.
   When the selected tracks have the same list of credits for a kind, the details pane shows every name and optional role. If the lists differ, it shows `<Multiple Values>` instead. The **Credits scope:** buttons open **Properties** directly at that scope.
   Missing artwork uses a monogram in group headings and a transparent vinyl placeholder in the details pane; these Windows choices are fixed in this version.
6. Use the persistent Now Playing controls for previous, play/pause, next, shuffle, repeat, seeking, volume, and output.
   Play in the window or from Windows media controls resumes a paused or restored track, even if another row is selected.
   When stopped with nothing to resume, Play starts the first selected track still visible in the current view, provided the output is available.
   With the seek slider focused, press Left or Right without modifiers to move five seconds per press. Repeated presses add up.
   If playback changes tracks or restarts the same track during a drag, release the slider and drag again to seek.
   If the total shows `--:--`, the elapsed time remains visible, but the slider is disabled.
   Press `Ctrl+L`, or choose **Reveal Current Track** from the Now Playing menu in Modern or **Playback** in Classic. Aobus selects and scrolls to the track in the List where playback started, using **All Tracks** if that List no longer contains it. If reveal changes views, `Alt+Left` returns to the view you left with its filter intact.
   Aobus remembers your output backend, device, and profile. If the named output you requested is missing at launch, Aobus waits rather than playing through the default device. Reconnect it or choose another output, then press Play.
   Now Playing can show a restored track's cover before you press Play. If there's no track to show or the track has no assigned cover, you see a transparent equalizer instead.
7. Choose **Classic Mode** to use the dense menu, tree, property, status, and GTK-compatible playback layout. Choose **Modern Mode** to return. Playback, the active library, list, and presentation continue across the switch.
8. In Modern mode, click Soul to play or pause. In Classic mode, click Soul to choose an output device.
   In either mode, right-click Soul for the system menu, hold it for full-screen Soul, or hover it to inspect the audio pipeline. See [interpreting playback quality](play-music.md#inspect-playback-quality) to distinguish source fidelity, pipeline changes, and incomplete evidence.
9. After files change, choose **More → Rescan Library** in Modern or **File → Rescan** in Classic. Wait until the command is available before requesting another scan; a second request isn't queued. If you switch libraries instead, the closing process requests cancellation and waits for its work to finish.
10. To back up portable library data, choose **More → Export Library Data...** in Modern mode or **File → Export Library Data...** in Classic mode, select Delta, Metadata, Full, or Lists only, and save a `.yaml` file.
    To import one, choose **Import Library Data...** from the same menu. Select **Merge (keep records outside the backup)** or **Restore (replace the backup scope after preview)**, choose **Next**, then select the backup file.
    The restore preview shows the backup version, mode, scope, change counts, and the number of ignored references and invalid List filters kept.
    To continue after reviewing the preview, choose **Restore Library** or **Restore Lists**, depending on the scope. **Cancel** is the default on that preview.
    The window lets you start only one import or export workflow at a time, and not while **Properties** or a List dialog is open.
    Modern shows transfer progress in its activity strip. Switching to Classic while a transfer is running does not cancel or restart it; Classic's status bar shows whether it succeeded or failed.
    Canceling the mode dialog or file picker, or declining the restore preview, leaves the library unchanged. Closing the window requests cancellation, not rollback. An import already submitted for applying may still commit.
11. To apply a custom theme, create `%LOCALAPPDATA%\Aobus\windows-theme.yaml` using the exact reference schema, then choose **Reload Theme**. Omit or remove the file to follow built-in and system appearance.

## Verify the result

The status area reports the selected library as ready, rows remain keyboard navigable, YAML transfers appear in Modern's activity surface, and Windows media keys and the system media overlay reflect the active track. Switching modes must not restart a scan or transfer or stop audio.

## Troubleshooting

- If Aobus restarts but cannot open the folder you chose, it reports **Aobus could not start:** and exits. A normal launch can reopen the previously saved library if it is still available. A scan failure, unlike an open failure, leaves the selected folder open for retry.
- If the root is on a network share, switching between a mapped drive letter and UNC spelling does not make it a supported database location. Use local storage; a remote desktop connection to Windows does not itself make that host's local disk a network filesystem.
- If theme reload fails, correct the reported field, type, or color. The last valid theme remains visible. Removing the file and choosing **Reload Theme** restores the Windows appearance.
- If the library changes while **Properties** is open, choose **Reload**, then **Discard Changes**, or close and reopen **Properties**. If you see **Library is busy. Try again.**, the dialog keeps your draft so you can retry.
- If you see **Import failed:**, check the notification details in Modern or the status bar in Classic (hover for the full message) for malformed YAML, an unsupported version, an unsafe path, or changes to the backup file or the library you are importing into since Aobus prepared the preview.
  If the import fails before Aobus saves it, none of the import's changes are applied.
- If no audio device appears, confirm that Windows Audio is running and that the desired endpoint is enabled.
- If media keys do not work in a remote session, verify them once in an interactive console or RDP desktop; redirected keys may be consumed by the client.

## Related reference

- [Windows desktop state](../reference/windows/desktop-state.md)
- [Windows desktop shell specification](../system/frontend/windows.md)
- [Edit metadata and Credits](edit-metadata.md)
- [Back up and restore library data](backup-and-restore.md)
- [Supported audio files](../reference/media/audio-file.md)
