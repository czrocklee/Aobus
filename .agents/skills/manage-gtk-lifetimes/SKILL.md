---
name: manage-gtk-lifetimes
description: Review or change app/linux-gtk/ widget lifetimes, signals, Gio actions, replaceable dependencies, view rebuilds, async callbacks, and popover first layout. Skip pure UIModel policy, non-GTK frontends, and static GTK layout or styling.
---

# Manage GTK lifetimes

Review is read-only unless fixes are requested.

- **Owners and rebuilds.** Name the C++ owner, GTK parent, emitter, and receiver,
  and what can replace or retire them. Keep each connection or attachment within
  the shortest lifetime it depends on. For rebuilds, trace
  [generation teardown](../../../doc/development/gtk-lifetime.md#view-generations)
  and check what [crosses lifetimes](../../../doc/development/gtk-lifetime.md#cross-lifetime-data).
  If a widget, gesture, bound action, or view generation would be destroyed in
  its own callback, queue retirement on the GTK main context so dispatch can
  unwind. The owner must be able to cancel the pending callback.
- **Signals and rebinding.** For [signal connections](../../../doc/development/gtk-lifetime.md#signal-connections),
  flag a synchronous or owner-bounded raw `this` capture only if you can show how
  the emitter outlives the receiver or calls back into it during teardown. A
  `sigc::scoped_connection` alone doesn't follow a replacement; check the
  [outer watcher, rebinding, and reconciliation](../../../doc/development/gtk-lifetime.md#replaceable-dependencies).
- **Actions.** For [action maps and attached groups](../../../doc/development/gtk-lifetime.md#action-maps-and-attached-groups),
  check that retirement disconnects handlers before removal and removes an action
  only if that exact object is still installed. A retained old action must be
  inert; the activation context and its window and anchor are synchronous borrows.
  An action handler must not destroy its registration synchronously.
- **Async completion.** For an [owner-member coroutine](../../../doc/development/gtk-lifetime.md#signal-connections)
  awaiting a runtime task, capture stable service input and create one
  `MainContextCallbackScope`-guarded presentation closure before suspension.
  After that `co_await`, invoke only the closure: no owner read or second
  main-context hop. For [dialog workflows](../../../doc/system/frontend/gtk/dialog-lifecycle.md#failure-and-cancellation),
  check presentation invalidation separately from cancellation and committed
  data delivery.
- **Popover lifetime and first layout.** Check explicit, idempotent detach.
  Render size-determining contents from current state before first presentation;
  a show or map handler can be too late. Keep contents current on every open and
  while open. See
  [transient attachments](../../../doc/development/gtk-lifetime.md#transient-attachments)
  and verify real first presentation through the popover's
  [own first native layout](../../../doc/development/test/uimodel-and-gtk.md#popover-first-layout),
  not just its attachment or an event drain.

Use [focused GTK lifecycle validation](../../../doc/development/gtk-lifetime.md#validation)
to prove replacement or retirement, not just construction. See
[Driving GTK behavior](../../../doc/development/test/uimodel-and-gtk.md#driving-gtk-behavior)
for how to exercise the affected path, and
[UIModel and GTK testing](../../../doc/development/test/uimodel-and-gtk.md)
for test layering and shared fixtures.
