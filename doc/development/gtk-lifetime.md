---
id: development.gtk-lifetime
type: development
status: current
domain: linux-gtk
summary: Defines lifetime-aligned GTK ownership, signal wiring, replaceable-source rebinding, view generations, and transient attachment policy.
---
# GTK lifetime and wiring

## Scope

This guide owns contributor policy for GTK object ownership, signal connections, replaceable dependencies, rebuildable widget generations, transient parenting, and delayed callbacks.
It applies to code under `app/linux-gtk/` and its GTK tests.

The [presentation architecture](../architecture/presentation.md) owns the runtime, UIModel, and frontend boundary.
The [application shell architecture](../architecture/application-shell.md) owns declarative shell construction and teardown.
This guide does not redefine those boundaries or own user-visible behavior.

## Policy

GTK wiring must align every connection and attachment with the shortest lifetime on which it depends.
A long-lived host owns stable state and signals, a replaceable view generation owns its widgets and controllers, and a transient attachment session owns a popover or other short-lived child only while its anchor remains valid.

```text
stable host or service
  -> disposable view generation
       -> transient attachment session
```

An inner scope must never outlive the outer scope that supplies its emitter, receiver, parent, model, adjustment, controller, or anchor.

### Signal connections

Store member-level GTK and sigc++ connections in `sigc::scoped_connection` or another owner-bounded subscription type.
Do not rely on member declaration order, a non-reentrant destruction sequence, or the assumption that the GLib main context will not run between teardown steps.

The connection lifetime must not exceed either endpoint's lifetime.
An ignored connection is acceptable only when automatic lifetime tracking covers the receiver, or ownership guarantees that the emitter is disconnected or destroyed before the receiver and cannot emit during teardown.
Document the non-obvious ownership and destruction order next to the connection.

Callbacks retained outside their owner use the repository's GTK callback-lifetime boundary, weak ownership, or explicit cancellation.
Use `MainContextCallbackScope` for void callbacks retained outside a GTK-main-context owner.
Use `sigc::track_object` for one-shot idle callbacks tied to a `Glib::ObjectBase` lifetime.
Neither mechanism replaces synchronization when callbacks can cross threads.

### Replaceable dependencies

When a GTK property or host slot can replace an observed object, use an outer watcher plus an inner connection scope:

1. Observe the property or slot that identifies the current object.
2. Disconnect every signal from the previous object.
3. Read and connect to the current object.
4. Reconcile the derived state immediately after replacement.

This pattern applies to adjustments, models, selection models, active controllers, visible children, roots, and frame clocks.
A scoped connection to the original object prevents a dangling callback but does not prevent stale wiring after replacement.

### View generations

Treat a rebuildable GTK tree as a disposable generation.
The generation owns its widgets, controllers, factories, generation-local models, signal forwarding, CSS providers, and transient attachments.

Retire a generation in this order:

1. Stop model or event delivery that can enter the retiring tree.
2. Close and unparent transient children anchored inside the tree.
3. Detach the root widget from its live parent.
4. Destroy the generation and its scoped connections as one unit.
5. Configure and attach the replacement generation.

Cross-generation communication uses stable ids, values, commands, or host-level signals.
Long-lived objects must not retain row widgets, cell widgets, adjustments, controllers, factories, or other generation-local pointers.

### Transient attachments

Every `set_parent()` in controller-owned or dynamically constructed UI requires an explicit, idempotent `unparent()` path owned by the same lifetime scope.
Closing, replacing, cancelling, and destroying a transient must all converge on that detach path.

Prefer attachment-session-owned transient widgets that unparent when closed.
A long-lived controller should retain business state and commands rather than a widget parented to a shorter-lived row, cell, page, or view generation.

Use `PopoverAttachment` for simple controller-owned, one-shot popovers.
It unparents on close and defers final destruction until the current GTK dispatch stack returns.
Keep stable member popovers and multi-stage popovers in their owning controller rather than forcing them through this helper.

Before replacing an owned popover or menu, pop it down and unparent the old instance.
Do not retain a raw anchor pointer after the anchor's generation can be closed or rebuilt.
Never destroy the dispatching widget, gesture, bound action, or view generation from inside its own synchronous callback; queue retirement on the GTK main context and make the queued callback owner-cancellable.

### Cross-lifetime data

Pass ids, immutable values, or narrow commands across lifetime boundaries.
A raw widget pointer may be used synchronously to present a transient but must not be stored beyond that operation unless an explicit attachment session proves and enforces the anchor lifetime.

Weak pointers prevent callbacks from dereferencing a destroyed C++ owner, but they do not repair a GTK parent relationship.
Scoped connections prevent late signal delivery, but they do not reconnect a replacement source.
Use the mechanism that enforces the actual ownership invariant.

## Workflow

1. Draw the emitter, receiver, GTK parent, C++ owner, and replacement path for each changed connection or attachment.
2. Classify every object as stable host, disposable generation, or transient attachment.
3. Store owner-level connections and subscriptions in a scoped member.
4. Add outer-watcher rebinding for every replaceable observed object.
5. Make attach and detach operations symmetric and idempotent before adding another presentation path.
6. Exercise replacement and teardown, not only initial construction.

Do not introduce a global wiring manager merely to centralize connections.
Prefer explicit ownership in the existing host, generation, or transient class; add a reusable helper only when multiple owners need the same complete lifecycle operation.

## Validation

GTK lifecycle changes require a focused regression test at the lowest GTK surface that can replace or retire the relevant object.

For a replaceable source, prove that the replacement drives updates and the retired source no longer does.
For a transient attachment, open or close it and then retire its anchor or generation; assert the semantic result and run without GTK finalization warnings or sanitizer failures.
For delayed callbacks, destroy the owner before completion and prove the callback becomes harmless.

Run the focused GTK test only while diagnosing the lifecycle path, then run the repository's normal `./ao check` completion gate.
Use the sanitizer workflow from the testing policy when the defect can dereference retired storage or a finalized GObject.

## Troubleshooting

If switching views, reopening a popover, resizing, or rebuilding makes a defect disappear, compare the replacement path with initial attachment before adding another refresh call.
The disappearing defect often indicates a stale observed object or a missed post-attachment reconciliation.

If GTK reports that a finalized widget still has children, find every application-level `set_parent()` targeting that widget and verify the matching detach path runs before parent finalization.

If a scoped connection is present but updates still stop after reparenting or model replacement, verify that the connection follows the property or host slot rather than the original object instance.

If teardown is safe only because several move assignments or destructors run in one exact order, move the connection or attachment into the disposable generation that owns both endpoints.

## Related documents

- [GTK style](gtk-style.md)
- [UIModel and GTK testing](test/uimodel-and-gtk.md)
- [Runtime and asynchronous testing](test/runtime-and-async.md)
- [Concurrency and sanitizer validation](test/concurrency-and-sanitizer.md)
- [Presentation architecture](../architecture/presentation.md)
- [Application shell architecture](../architecture/application-shell.md)
- [GTK dialog lifecycle](../spec/linux-gtk/dialog-lifecycle.md)
