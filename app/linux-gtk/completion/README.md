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
- The controller borrows `Gtk::Entry&`; caller code keeps the entry alive until the controller is destroyed.
- The controller removes every key, click, and focus controller it installed on that borrowed entry before its own callbacks can be released. This matters when a longer-lived entry replaces its completion provider.
- `setTextProgrammatically()` blocks the controller's changed connection, updates the entry, and clears completion state; callers use it when synchronizing external state without querying the provider or reopening the popover.
- Up and Down cycle through the transient rows, while Page Up and Page Down move by the visible page and stop at its boundaries.
- Tab applies the selected replacement.
- Return normally dismisses the popover without replacing the borrowed entry's text or consuming its host action; a host may explicitly opt into Return acceptance, as the track Quick Filter does.
- Escape dismisses the popover without applying its selected replacement.
- Left, Right, Home, and End dismiss the visible popover and continue to the entry's ordinary caret handling.
- Pointer activation applies the selected replacement.

## Evidence

- `EntryCompletionController.h` owns the borrowed-entry contract, installed controller handles, scoped changed connection, and programmatic update API.
- `EntryCompletionController.cpp` owns key routing, checked byte-range replacement, signal blocking, and teardown ordering.
- `test/unit/linux-gtk/completion/EntryCompletionControllerTest.cpp` protects replacement and programmatic-update behavior.
