# GTK Dialogs

Aobus custom GTK dialogs use `ao::gtk::AppDialog`, which owns a `Gtk::HeaderBar` and places dialog actions such as
`Cancel`, `Close`, `Apply`, and `Save` in that header bar.

Custom dialogs hide the header bar's window-manager title buttons. The visible action buttons are therefore the only
in-dialog controls for closing, canceling, applying, or saving. Native GTK dialogs such as `Gtk::FileDialog`,
`Gtk::AboutDialog`, and `Gtk::MessageDialog` remain controlled by GTK and the desktop environment.

## Window Classification

Preferences live in one non-modal, application-owned `PreferencesWindow`, opened by
`app.preferences` (`Ctrl+,`) and by **Edit → Preferences…**. Object editors such as
`SmartListDialog`, `TrackPropertiesDialog`, `TrackCustomViewDialog`, and
`LayoutEditorDialog` keep their modal commit/cancel semantics. Transient messages
and confirmations use `AppDialog::presentMessage` or native GTK dialogs.

The first Preferences surface contains General, Appearance, Playback/Output,
Layout, and Keyboard pages. Appearance theme changes apply immediately and are
persisted as application preferences. Playback output changes use the playback
engine's confirmed selected device before persisting. Layout default preset
selection is persisted for the next layout load; immediate layout edits still go
through `LayoutEditorDialog`, and runtime-state reset / panel-size promotion keep
their confirmation flow.

## Open Library

`File -> Open Library...` uses a native `Gtk::FileDialog` folder chooser. Selecting a real library from a normal
library window opens an additional main window. If the source window was the startup fallback empty library window,
the selected library opens in a new main window and the fallback window is removed so the user experiences it as a
replacement rather than an extra empty window.

When the selected folder does not already contain an Aobus database, the newly opened library window starts an initial
scan immediately. Scan progress and completion are surfaced through the activity/notification UI; there is no modal
progress dialog. After scan or YAML import mutates library data, the active window reloads sources and rebuilds the
track/list views so newly imported tracks appear without restarting.

## Layout Editor

The layout editor is an object editor: layout changes are committed through `Save`, previewed through
`Apply`, or abandoned through `Cancel`.

Its theme selector is preview-only. Choosing a theme in the editor immediately previews the layout
under that theme, but the editor does not persist theme preferences. `Cancel` rolls the preview back
to the theme that was active when the editor opened, and `Save` restores the active theme from the
persisted application preference after saving the layout. Theme persistence belongs to Preferences.
