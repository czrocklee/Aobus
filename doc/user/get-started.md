---
id: user.get-started
type: user-guide
status: current
domain: workspace
summary: Opens a music root, builds its library index, and starts playback in the GTK application.
---
# Get started with Aobus

## Outcome

You have one music folder open as the active Aobus library, its supported audio files appear in the track view, and you can start playback.

## Before you start

Aobus treats the folder you choose as the music root.
Its library database is stored below that root at `.aobus/library`; your audio files remain in their existing folders.
The current scan recognizes the formats listed in the [audio-file reference](../reference/media/audio-file.md#supported-extensions-and-codecs).

## Steps

1. Start the GTK application:

   ```bash
   aobus-gtk
   ```

2. Open **File → Open Library...** and choose the folder that contains your music.
   Aobus activates that folder as the one current library.
   Choosing another folder later replaces the active library in the same application window; it does not open a second independent library window.
3. If the root has no existing Aobus database, wait for the bootstrap scan to finish.
   The activity status shows scan progress and the final result.
4. If you opened an existing database and want to reconcile it with the files on disk, choose **File → Scan Library**.
5. Select a track and press Enter, or activate it with the pointer, to start playback from the current track view.

## Verify the result

- The track view contains the supported files beneath the selected root.
- The activity status reports a completed scan or says that the library is up to date.
- Starting a track updates the now-playing surface and transport controls.

Files with unsupported extensions are skipped rather than reported as tracks.
For scan classification and metadata-refresh behavior, see [library scan and audio identity](../spec/library/runtime/scan-and-identity.md).

## Related documents

- [Manage a library](manage-library.md)
- [Play music](play-music.md)
- [Active-library lifecycle](../spec/linux-gtk/active-library-lifecycle.md)
- [Library database location](../reference/persistence/location.md)
