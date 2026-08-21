# linux-gtk/completion

This directory binds runtime completion providers to GTK entries.

## Roles

- **Port:** `ao::rt::CompletionProvider` and `ao::rt::CompletionResult` define the UI-neutral completion contract.
- **Adapter:** `EntryCompletionController` renders provider results in a `Gtk::Popover` and applies the selected replacement to a borrowed `Gtk::Entry`.
- **Driver:** GTK callers own the relevant runtime completer and pass its provider into the controller.

## Invariants

- The controller does not own vocabulary data; `ao::rt::CompletionService` owns and refreshes vocabularies.
- The controller does not parse query syntax; `ao::rt::QueryExpressionCompleter` owns query context analysis.
- The controller's `Gio::ListModel` rows are transient rendered `CompletionItem` objects; there is no parallel string/index model.
- The controller borrows `Gtk::Entry&`; caller code owns the entry lifetime.
- Up and Down cycle through the transient rows, while Page Up and Page Down move by the visible page and stop at its boundaries.
- Tab applies the selected replacement.
- Return normally dismisses the popover without replacing the borrowed entry's text or consuming its host action; a host may explicitly opt into Return acceptance, as the track Quick Filter does.
- Escape dismisses the popover without applying its selected replacement.
- Pointer activation applies the selected replacement.
