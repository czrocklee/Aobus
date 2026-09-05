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
The active-library switch and successor restart belong to the [GTK active-library lifecycle specification](active-library-lifecycle.md).

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
- Saved-List create/edit dialogs remain open with their draft intact when submission fails and close only after success or cancellation.
- Native chooser cancellation is a no-op.
- The export-mode response and native folder, open, and save completions can access `ImportExportCoordinator` only while its callback scope remains live.
- Open Library selecting the active normalized root reuses and presents the current window; selecting a different valid root requests a destructive successor-process restart.
- The Open Library dialog does not create an additional independent main-window/library pair.
- A new library root starts bootstrap scan after successful activation when the open policy requests it.
- Long-running scan/import progress uses activity and notification surfaces, not a modal progress dialog.
- A scan, backfill, import, export, or preview coroutine captures its stable task-service input and creates one `MainContextCallbackScope`-guarded presentation or confirmation closure before suspension.
- After awaiting the task, the coroutine invokes only that pre-created closure: it does not dereference the workflow owner, add another executor hop, or refresh committed data outside `LibraryChanges`.

## State model

Preferences retains current application preference models while visible and applies supported settings through their owners.
The Keyboard page additionally retains the last successfully applied keymap and any failed save candidate.
The layout editor retains a document draft, preview state, and the theme that was active when it opened.
The native folder chooser retains GTK async operation state until completion.
`ImportExportCoordinator` retains one callback scope for its export-mode response and native folder, open, and save completions, plus one cancellable shared by the native operations.

## Commands and transitions

Preferences opens from `app.preferences`, `Ctrl+,`, or the Edit menu.
Appearance changes apply immediately and persist through application preferences.
Keyboard changes persist the candidate before publishing live accelerators.
Output changes persist only after the playback path confirms the selected device.
Default layout-preset choice affects the next layout load; structural edits use the layout editor.
Ordinary close of Preferences with a failed shortcut candidate switches to the Keyboard page and prompts Retry, Discard, or keep editing.
Repeated close requests reuse the live prompt rather than stacking another one.
Closing the prompt itself answers it with keep editing, never with Discard.
Reopening Preferences while that candidate is pending keeps the existing Keyboard editor.
Hiding the target application window dismisses Preferences without that prompt and retires any live prompt, whose later response no longer steers the window.

Open Library launches a native folder chooser.
A successful folder selection is normalized and handed to the single active-library host.
Same-root selection may trigger a fast bootstrap scan and presents the existing window.
Different-root selection returns from the native completion, terminally retires the old playback session, fully releases the old pair and GTK composition, and then launches a strict explicit-root successor.
The successor activates with idle playback, records the new global path best effort, and optionally begins the carried bootstrap scan.

The layout editor uses:

- `Apply` to preview the current draft;
- `Save` to request persistence, close only after that workflow succeeds, and then restore the persisted application theme;
- `Cancel` to abandon the draft and restore the theme active when the editor opened.

Its theme selector is preview-only; theme persistence belongs to Preferences.

## Failure and cancellation

Native file-dialog cancellation or dismissal is silent and creates no replacement.
Other native file-dialog failures are logged and presented in a parent-bound transient message at the initiating window.
A saved-List preview-source acquisition failure uses the editor's existing error label.
The saved-List editor evaluates each non-empty local expression through a leased ad-hoc runtime source: a valid expression drives the transient preview projection, a local expression failure hides that preview and shows its diagnostic, and a valid expression over a broken saved-List parent chain shows the propagated contextual parent error.
Saved-List create or edit rejection retains the draft and editor, while delete rejection presents a parent-bound transient message without changing the tree or selection.
Deferred track-presentation selection retains the target view identity; if focus changes before application or the runtime rejects the change, it leaves the current view unchanged and presents a parent-bound transient message.
Active-pair terminal-retirement failure presents a parent-bound transient message and leaves the old pair visible.
After retirement, process-launch or successor-startup failure presents a standalone native diagnostic without reconstructing the old pair, as specified by the active-library lifecycle.
A Keyboard-page shortcut persistence failure retains the candidate, leaves live accelerators unchanged, and reports the error once on that page.
Layout-editor validation or persistence failure retains the draft and keeps the editor open.
A persistence failure presents a transient error message; partial multi-preset persistence and retry behavior belong to the [shell layout lifecycle](../shell/layout-lifecycle.md).

Destroying a parent window also destroys its application-owned child dialogs and releases their signal connections; a native file dialog can retain its GTK-owned async state until the toolkit completion runs.
Object-editor cancellation is explicit draft abandonment, not runtime cancellation of an already committed command.
Destroying `ImportExportCoordinator` first invalidates its callback scope and then requests cancellation through its shared `Gio::Cancellable`.
A custom export-mode response or native completion delivered after invalidation is a no-op and cannot launch, finish, or hand a selected path through the destroyed coordinator.
Cancellation is best-effort cleanup rather than the memory-safety proof.
Destroying `LibraryImportExportWorkflow` closes its presentation scope before cancelling spawned tasks.
An awaited task may still finish its runtime cleanup, but its guarded presentation closure becomes a no-op and cannot touch the destroyed workflow.
Optional progress, item-failure, and presentation callbacks contain and log their own exceptions without changing the task Result.

## Persistence and versioning

Preferences persists through application managed state.
Layout document, component state, shortcut, and active-library path formats belong to their focused references.
`AppDialog` itself owns no serialized state.

## Frontend observations

The Preferences surface contains General, Appearance, Playback/Output, Layout, and Keyboard pages.
Messages and confirmations may use `AppDialog::presentMessage` or a native GTK dialog according to whether application-owned actions are required.

## Implementation map

- [`AppDialog.cpp`](../../../app/linux-gtk/app/AppDialog.cpp) owns custom-dialog chrome, actions, and parent-bound destruction.
- [`PreferencesWindow.cpp`](../../../app/linux-gtk/preference/PreferencesWindow.cpp) owns the non-modal preferences surface and pending-shortcut close confirmation.
- [`ImportExportCoordinator.cpp`](../../../app/linux-gtk/portal/ImportExportCoordinator.cpp) owns native chooser handoff.
- [`LibraryImportExportWorkflow.cpp`](../../../app/linux-gtk/portal/LibraryImportExportWorkflow.cpp) owns guarded post-await task presentation and delegates committed refresh to `LibraryChanges`.
- [`MainContextCallbackScope.h`](../../../app/linux-gtk/common/MainContextCallbackScope.h) owns main-context callback-lifetime validation; `ImportExportCoordinator` supplies native cancellation as its close action.
- [`main.cpp`](../../../app/linux-gtk/main.cpp) owns the active-library restart handoff.
- [`LayoutEditorDialog.cpp`](../../../app/linux-gtk/layout/editor/LayoutEditorDialog.cpp) owns editor preview and commit interaction.

## Test map

- [`PreferencesWindowTest.cpp`](../../../test/unit/linux-gtk/preference/PreferencesWindowTest.cpp) protects page construction, preference persistence, pending-candidate retention on reopen, the close prompt's Retry, Discard, Cancel, and close-request responses, single-prompt reuse and retirement on dismissal, and shortcut-candidate close versus target-hide dismissal.
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
