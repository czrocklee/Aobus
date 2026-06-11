# Tag Editor Layout

Current-tag chips use `Gtk::Box` for label and remove-button layout. The label ellipsizes when horizontal
space is constrained, while the remove button keeps a 20-pixel minimum allocation and remains clickable.
This delegates measurement and allocation to GTK instead of duplicating box-layout rules in a custom
widget implementation.

The editor itself remains height-for-width and horizontally compressible so it can live in narrow detail
panes. GTK tests cover long tag labels, narrow chip allocation, and remove-button interaction.
