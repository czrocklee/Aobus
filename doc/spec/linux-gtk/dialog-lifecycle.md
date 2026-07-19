---
id: linux-gtk.dialog-lifecycle
type: spec
status: current
domain: presentation
summary: Defines GTK custom-dialog roles, preferences lifetime, native chooser boundaries, Open Library handoff, and layout-editor commit semantics.
---
# GTK dialog-lifecycle specification

## Scope

This specification owns GTK window/dialog classification and the interaction lifetime of application-owned dialogs.
It also defines the native Open Library chooser handoff and layout-editor apply/save/cancel semantics.
The active runtime replacement itself belongs to the [GTK active-library lifecycle specification](active-library-lifecycle.md).

## Code boundary

`AppDialog` owns the shared custom-dialog header bar, content area, actions, response mapping, and transient message helper.
Application-owned `PreferencesWindow` is a non-modal top-level GTK window.
Object editors such as `SmartListDialog`, `TrackPropertiesDialog`, `TrackCustomViewDialog`, and `LayoutEditorDialog` retain modal commit/cancel semantics.
Native `Gtk::FileDialog`, `Gtk::AboutDialog`, and `Gtk::MessageDialog` remain desktop/GTK-owned.

No dialog directly replaces `AppRuntime` or mutates a shell layout store outside its owning workflow.

## Terminology

- An **application window** is a reusable, non-modal application-owned top level.
- An **object editor** edits one draft and commits or cancels it as a unit.
- A **transient message** reports or confirms an operation without owning durable state.
- **Apply** previews a layout draft without making it the stored final document.

## Invariants

- Custom dialogs place visible cancel/close/apply/save actions in their `AppDialog` header bar and hide window-manager title buttons.
- Application-owned modal child dialogs are transient for and destroyed with their parent window.
- Preferences has one application-owned instance and is non-modal.
- Object editors do not commit a draft on cancel or ordinary close.
- Native chooser cancellation is a no-op.
- The export-mode response and native folder, open, and save completions can access `ImportExportCoordinator` only while its callback scope remains live.
- Open Library selecting the active normalized root reuses and presents the current window; selecting a different valid root replaces the active library/window pair.
- The Open Library dialog does not create an additional independent main-window/library pair.
- A new library root starts bootstrap scan after successful activation when the open policy requests it.
- Long-running scan/import progress uses activity and notification surfaces, not a modal progress dialog.

## State model

Preferences retains current application preference models while visible and applies supported settings through their owners.
The layout editor retains a document draft, preview state, and the theme that was active when it opened.
The native folder chooser retains GTK async operation state until completion.
`ImportExportCoordinator` retains one callback scope for its export-mode response and native folder, open, and save completions, plus one cancellable shared by the native operations.

## Commands and transitions

Preferences opens from `app.preferences`, `Ctrl+,`, or the Edit menu.
Appearance changes apply immediately and persist through application preferences.
Output changes persist only after the playback path confirms the selected device.
Default layout-preset choice affects the next layout load; structural edits use the layout editor.

Open Library launches a native folder chooser.
A successful folder selection is normalized and handed to the single active-library host.
Same-root selection may trigger a fast bootstrap scan and presents the existing window.
Different-root selection prepares the current pair, removes it, records the new global path, constructs one replacement pair, configures its callbacks, and optionally begins bootstrap scan.

The layout editor uses:

- `Apply` to preview the current draft;
- `Save` to request persistence, close only after that workflow succeeds, and then restore the persisted application theme;
- `Cancel` to abandon the draft and restore the theme active when the editor opened.

Its theme selector is preview-only; theme persistence belongs to Preferences.

## Failure and cancellation

Native file-dialog cancellation or dismissal is silent and creates no replacement.
Other native file-dialog `Glib::Error` values are logged by the frontend owner.
Active-library preparation failure leaves the old pair visible, as specified by the active-library lifecycle.
Layout-editor validation or persistence failure retains the draft and keeps the editor open.
A persistence failure presents a transient error message; partial multi-preset persistence and retry behavior belong to the [shell layout lifecycle](../shell/layout-lifecycle.md).

Destroying a parent window also destroys its application-owned child dialogs and releases their signal connections; a native file dialog can retain its GTK-owned async state until the toolkit completion runs.
Object-editor cancellation is explicit draft abandonment, not runtime cancellation of an already committed command.
Destroying `ImportExportCoordinator` first invalidates its callback scope and then requests cancellation through its shared `Gio::Cancellable`.
A custom export-mode response or native completion delivered after invalidation is a no-op and cannot launch, finish, or hand a selected path through the destroyed coordinator.
Cancellation is best-effort cleanup rather than the memory-safety proof.

## Persistence and versioning

Preferences persists through application managed state.
Layout document, component state, shortcut, and active-library path formats belong to their focused references.
`AppDialog` itself owns no serialized state.

## Frontend observations

The Preferences surface contains General, Appearance, Playback/Output, Layout, and Keyboard pages.
Messages and confirmations may use `AppDialog::presentMessage` or a native GTK dialog according to whether application-owned actions are required.

## Implementation map

- [`AppDialog.cpp`](../../../app/linux-gtk/app/AppDialog.cpp) owns custom-dialog chrome, actions, and parent-bound destruction.
- [`PreferencesWindow.cpp`](../../../app/linux-gtk/preference/PreferencesWindow.cpp) owns the non-modal preferences surface.
- [`ImportExportCoordinator.cpp`](../../../app/linux-gtk/portal/ImportExportCoordinator.cpp) owns native chooser handoff.
- [`MainContextCallbackScope.h`](../../../app/linux-gtk/common/MainContextCallbackScope.h) owns main-context callback-lifetime validation; `ImportExportCoordinator` supplies native cancellation as its close action.
- [`main.cpp`](../../../app/linux-gtk/main.cpp) owns active-library replacement.
- [`LayoutEditorDialog.cpp`](../../../app/linux-gtk/layout/editor/LayoutEditorDialog.cpp) owns editor preview and commit interaction.

## Test map

- [`AppDialogTest.cpp`](../../../test/unit/linux-gtk/app/AppDialogTest.cpp) protects common window composition, parent-bound destruction configuration, and messages.
- [`MainContextCallbackScopeTest.cpp`](../../../test/unit/linux-gtk/common/MainContextCallbackScopeTest.cpp) protects callback invalidation before the configured close action.
- [`ImportExportCoordinatorTest.cpp`](../../../test/unit/linux-gtk/portal/ImportExportCoordinatorTest.cpp) protects chooser handoff, scan policy, and export-mode response invalidation.
- Layout-editor tests under [`test/unit/linux-gtk/layout/editor/`](../../../test/unit/linux-gtk/layout/editor/) protect draft, preview, and action behavior.
- [`ShellLayoutControllerTest.cpp`](../../../test/unit/linux-gtk/app/ShellLayoutControllerTest.cpp) protects persistence-failure feedback and editor retention.

## Related documents

- [Application shell architecture](../../architecture/application-shell.md)
- [Presentation architecture](../../architecture/presentation.md)
- [GTK active-library lifecycle](active-library-lifecycle.md)
- [Shell layout lifecycle](../shell/layout-lifecycle.md)
- [Application managed-state reference](../../reference/persistence/application-config.md)
- [RFC 0026: lifetime-safe GTK file-dialog callbacks](../../rfc/0026-lifetime-safe-file-dialog-callbacks.md)
