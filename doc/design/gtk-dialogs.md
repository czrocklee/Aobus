# GTK Dialogs

Aobus custom GTK dialogs use `ao::gtk::AppDialog`, which owns a `Gtk::HeaderBar` and places dialog actions such as
`Cancel`, `Close`, `Apply`, and `Save` in that header bar.

Custom dialogs hide the header bar's window-manager title buttons. The visible action buttons are therefore the only
in-dialog controls for closing, canceling, applying, or saving. Native GTK dialogs such as `Gtk::FileDialog`,
`Gtk::AboutDialog`, and `Gtk::MessageDialog` remain controlled by GTK and the desktop environment.

## Open Library

`File -> Open Library...` uses a native `Gtk::FileDialog` folder chooser. Selecting a real library from a normal
library window opens an additional main window. If the source window was the startup fallback empty library window,
the selected library opens in a new main window and the fallback window is removed so the user experiences it as a
replacement rather than an extra empty window.

When the selected folder does not already contain an Aobus database, the newly opened library window starts an initial
scan immediately. Scan progress and completion are surfaced through the activity/notification UI; there is no modal
progress dialog. After scan or YAML import mutates library data, the active window reloads sources and rebuilds the
track/list views so newly imported tracks appear without restarting.
