---
id: user.windows-desktop
type: user-guide
status: current
domain: application-shell
summary: Opens, browses, and plays a music library with the native Windows desktop frontend.
---
# Use the Windows desktop

## Outcome

You will open an indexed music folder in the native Windows application, browse and play tracks, switch between Modern and Classic layouts without interrupting playback, and optionally reload a semantic theme.

## Prerequisites

- Windows 11 24H2 or Windows Server 2025 on x64.
- The unpackaged, framework-dependent Aobus WinUI build and its Windows App Runtime dependency.
- A local or reachable folder containing supported music files.

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
   views, and the other built-in presentations. Grouped presentations add
   album or category headings without turning those headings into playable
   rows.
4. Type in **Filter tracks** to narrow the active view. Select one or more rows
   with ordinary Windows list gestures, use a sortable column header to change
   ordering, and double-click a track row to start playback. Drag a header's
   right edge to resize it, right-click a header to move it, and use **More >
   Columns** in Modern or **View > Columns** in Classic to show or hide fields.
5. Inspect the selected track in the details pane. Drag either inline pane
   boundary to resize navigation or details; Aobus remembers both widths. On
   medium or narrow windows, use the details button to open it as an overlay;
   the navigation pane becomes compact or minimal automatically.
   Missing artwork uses a monogram in group headings and a transparent vinyl placeholder in the details pane; these Windows choices are fixed in this version.
6. Use the persistent Now Playing controls for previous, play/pause, next, shuffle, repeat, seeking, volume, and output.
   Aobus remembers the output backend, device, and profile you request and tries
   to restore them after providers are discovered on the next launch. A named
   device may remain preferred while disconnected; until it can be selected,
   the runtime-selected default remains active.
   Missing Now Playing artwork and idle playback use a transparent equalizer placeholder.
7. Choose **Classic Mode** to use the dense menu, tree, property, status, and GTK-compatible playback layout. Choose **Modern Mode** to return. Playback, the active library, list, and presentation continue across the switch.
8. In Classic mode, click Soul for output devices, right-click it for the system menu, hold it for full-screen Soul, or hover it to inspect the audio pipeline.
9. Choose **Rescan** after files change. A failed scan leaves the active library open and retryable. Another Rescan while one is active starts nothing; choosing a different library instead closes the current process and cancels its work during teardown.
10. To apply a custom theme, create `%LOCALAPPDATA%\Aobus\windows-theme.yaml` using the exact reference schema, then choose **Reload Theme**. Omit or remove the file to follow built-in and system appearance.

## Verify the result

The status area reports the selected library as ready, rows remain keyboard navigable, and Windows media keys and the system media overlay reflect the active track. Switching modes must not restart the scan or stop audio.

## Troubleshooting

- If a selected folder cannot be opened, the successor process reports startup failure and exits; start Aobus normally to return to the previously saved library. A scan failure instead leaves the selected library open and retryable.
- A mapped drive must be visible in the same interactive Windows sign-in that launches Aobus.
  If a service or SSH session cannot see the drive letter, select the equivalent UNC path instead.
- If theme reload fails, correct the reported field, type, or color. The last valid theme remains visible. Removing the file and choosing **Reload Theme** restores the Windows appearance.
- If no audio device appears, confirm that Windows Audio is running and that the desired endpoint is enabled.
- If media keys do not work in a remote session, verify them once in an interactive console or RDP desktop; redirected keys may be consumed by the client.

## Related reference

- [Windows desktop state](../reference/windows/desktop-state.md)
- [Windows desktop shell specification](../spec/shell/windows-desktop.md)
- [Supported audio files](../reference/media/audio-file.md)
