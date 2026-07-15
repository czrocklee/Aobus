---
id: rfc.0026.generation-bound-platform-requests
type: rfc
status: implemented
domain: presentation
summary: Introduced a coordinator-scoped lifetime guard for GTK file-dialog callbacks that may complete after teardown.
depends-on: none
---
# RFC 0026: Lifetime-safe GTK file-dialog callbacks

## Disposition

Implemented on 2026-07-15 with the narrow GTK-local design described below.

`ImportExportCoordinator` now owns one GTK-local `MainContextCallbackScope` that guards its export-mode response and all folder, open, and save completions, and supplies the native operations with their shared cancellation handle.
Coordinator teardown closes the guard before requesting cancellation, so a retained callback delivered afterward is a no-op even if toolkit cancellation loses the race.

The [presentation architecture](../architecture/presentation.md) and [interactive session lifecycle architecture](../architecture/interactive-session-lifecycle.md) own the current structural boundary.
The [GTK dialog-lifecycle specification](../spec/linux-gtk/dialog-lifecycle.md) and [GTK active-library lifecycle specification](../spec/linux-gtk/active-library-lifecycle.md) own exact current behavior.
Those authorities supersede this proposal; this RFC retains the focused rationale and rejected broader designs.

## Problem

Before this RFC, `ImportExportCoordinator` started asynchronous `Gtk::FileDialog` folder, open, and save operations whose callbacks captured `this`.
The coordinator had a default destructor and no owner-lifetime token or native cancellation handle.
A callback delivered after its window and coordinator have been destroyed can therefore dereference a stale owner before the existing workflow lifetime protection begins.
The export workflow also connected an `AppDialog` response lambda that captured the coordinator before launching the native save dialog, without tracking or guarding that owner.

`LibraryImportExportWorkflow` already lifetime-binds the worker work started after a path is selected.
It cannot protect the earlier native callback that calls into the coordinator to hand off that path.

The coordinator is owned by `MainWindowCoordinator` and belongs to one window/runtime pair.
Replacing that pair or closing the application destroys the coordinator, so its lifetime already provides the freshness boundary required by the current composition.
GTK main-context delivery provides executor affinity but does not by itself prove that the coordinator remains alive.

The repository also had no deterministic test that retained a guarded native completion, destroyed its owner scope, and then delivered the completion.
This RFC addresses that focused lifetime gap rather than introducing a general platform-request protocol.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: None.

The implementation is confined to the GTK portal adapter.
It does not change active-library transaction semantics, runtime execution, resource delivery, or expression/library behavior.

## Goals

- Prevent the export-mode response and native folder, open, and save completions from dereferencing a destroyed `ImportExportCoordinator`.
- Make a completion delivered after coordinator teardown a safe no-op.
- Request native cancellation during teardown when the supported GTK API permits it, without relying on cancellation for memory safety.
- Preserve the current successful Open Library, import, and export handoffs.
- Keep the lifetime mechanism GTK-local and explicitly confined to one GLib main context.
- Add deterministic tests that deliver captured completions before and after owner teardown without requiring a real desktop portal.

## Non-goals

- Define a platform-neutral request framework or add a new core/runtime abstraction.
- Add frontend, runtime, library, or resource generation counters.
- Replace existing sigc++ lifetime tracking where a callback already tracks its actual owner.
- Change whether concurrent chooser requests are independent, latest-wins, or single-flight.
- Redefine active-library replacement or `LibraryImportExportWorkflow` cancellation.
- Add persistence or diagnostics infrastructure for pending requests.

## Implemented design

### Coordinator lifetime is the authority

One `ImportExportCoordinator` belongs to one `MainWindowCoordinator` and its window/runtime pair.
The coordinator's live callback scope therefore means that a guarded response or native completion may still act on that pair.
`MainContextCallbackScope` is the coordinator's last member, so reverse member destruction invalidates it before the cancellable, workflow, or borrowed window/runtime references are destroyed.

No separate generation value is needed while replacing a window/runtime pair also replaces its coordinator.
If a future composition retargets one live coordinator to another runtime, that new behavior must define its own freshness contract rather than being anticipated here.

### Guarded coordinator callbacks

`MainContextCallbackScope` owns a small shared state token and guards arbitrary void callbacks rather than depending on `Gio::SlotAsyncReady`.
The response or completion slot installed into GTK captures a weak reference to that token and the original slot.
The original completion may capture:

- the `Gtk::FileDialog` reference required to finish the operation;
- immutable operation values such as export mode; and
- the coordinator address, whose use remains conditional on successfully locking the live token.

The outer native slot performs no coordinator, window, workflow, runtime, or library dereference before validating the token.
The coordinator, its destruction, and the callback remain GTK-main-context confined, making token validation and owner invocation one serialized operation rather than a cross-thread synchronization protocol.

Completion follows this order:

1. attempt to lock the coordinator's callback-scope token;
2. drop the completion immediately when the token is closed;
3. otherwise finish the native GTK operation using the retained dialog reference;
4. translate a successful result into an owned path value; and
5. hand the path to Open Library or the existing import/export workflow.

A closed guard drops the response or result without logging a user-visible failure or starting work.
Ordinary operational errors delivered while the owner is live retain the current frontend-local reporting behavior.
Expected chooser cancellation remains a no-op.

### Teardown and cancellation

The coordinator owns the callback scope as its last member and retains its default destructor.
The scope destructor first resets the shared state token and then invokes its optional close callback.
`ImportExportCoordinator` supplies a close callback that cancels one `Gio::Cancellable` shared by every native file-dialog operation it owns.

Cancellation is cleanup and responsiveness, not the correctness proof: a toolkit callback may still run after cancellation, and the closed lifetime guard must still make it harmless.
The destructor does not wait for the native portal or run a nested main loop.

Once a live completion starts `LibraryImportExportWorkflow`, the workflow's existing `LifetimeScope` continues to own cancellation across worker and callback-executor hops.
This RFC does not add a second task-lifetime mechanism.

### Request behavior

The export-mode response and each chooser keep their current product behavior.
This RFC does not add request ids, supersession, coalescing, or a multi-state terminal machine.
The guarded delivery is used once for the one native completion, and any completion observed after the guard closes is ignored.

Tests directly retain a guarded callback and invoke it before and after the scope closes.
The close callback invokes the retained callback during teardown to prove invalidation happens before native cleanup.
No portal launcher abstraction or real desktop portal is required.

## Alternatives

### Cancel native operations only

Cancellation can race with completion and may still result in a toolkit callback.
Owner-lifetime validation is required even when cancellation is available.

### Add explicit frontend/runtime/library generations

The current coordinator is reconstructed with every window/runtime replacement, so its lifetime already distinguishes the old pair from the new pair.
Additional counters and evidence structures would duplicate that boundary without fixing another demonstrated case.

### Make the coordinator shared-owned

Changing `MainWindowCoordinator` composition to retain `ImportExportCoordinator` through `shared_ptr` would keep the entire coordinator and its borrowed runtime graph reachable for a late native callback.
A small invalidatable callback token preserves the existing ownership model and prevents late work instead.

### Rely on GTK main-context affinity

Serialization prevents simultaneous main-context callbacks but does not extend object lifetime.
The callback still needs an explicit proof before owner access.

## Compatibility and migration

No persisted format, command, or normal successful dialog behavior changes.
Late completions that previously had no safe contract become no-ops after their coordinator is gone.

`ImportExportCoordinator` migrates its owner-capturing dialog callbacks to the GTK-local callback scope, and `ShortcutEditorWidget` replaces its equivalent hand-written alive token with the same primitive.
Other platform adapters remain unchanged, and callbacks that already track their actual owner through sigc++ continue to use that tracking rather than this scope.

## Validation

- A guarded completion delivered while its scope is live invokes its target once.
- Scope destruction invalidates retained callbacks before cancelling the native operation.
- Repeated callback delivery after scope destruction performs no target access or side effect.
- An export-mode response delivered after coordinator teardown does not close the retained dialog or launch a native save operation.
- Folder, open, and save launch sites all use the same guarded slot and cancellable boundary.
- Expected chooser cancellation and dismissal remain silent; operational errors retain one frontend-local logging path.
- Focused lifetime tests use controlled callback delivery rather than timing sleeps or a real desktop portal.
- AddressSanitizer and ThreadSanitizer validation cover the guarded lifetime contract, followed by a full `./ao check`.

## Open questions

None for the implemented boundary.
A future coordinator that is retargeted while remaining alive, or a callback delivered across threads, requires a separate freshness or synchronization contract.

## Promotion plan

The implemented current behavior is owned by:

- [Presentation architecture](../architecture/presentation.md)
- [Interactive session lifecycle architecture](../architecture/interactive-session-lifecycle.md)
- [GTK dialog-lifecycle specification](../spec/linux-gtk/dialog-lifecycle.md)
- [GTK active-library lifecycle specification](../spec/linux-gtk/active-library-lifecycle.md)

No platform-neutral request abstraction or reference document was created because the implementation adds no persisted format, command surface, or cross-frontend protocol.
