# GTK Dialogs

Aobus custom GTK dialogs use `ao::gtk::AppDialog`, which owns a `Gtk::HeaderBar` and places dialog actions such as
`Cancel`, `Close`, `Apply`, and `Save` in that header bar.

Custom dialogs hide the header bar's window-manager title buttons. The visible action buttons are therefore the only
in-dialog controls for closing, canceling, applying, or saving. Native GTK dialogs such as `Gtk::FileDialog`,
`Gtk::AboutDialog`, and `Gtk::MessageDialog` remain controlled by GTK and the desktop environment.
