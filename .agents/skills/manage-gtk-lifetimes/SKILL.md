---
name: manage-gtk-lifetimes
description: Review or change app/linux-gtk/ widget lifetimes, signal connections, action groups, replaceable dependencies, view generations, and transient popovers. Do not load for pure UIModel policy, non-GTK frontends, or static GTK layout/styling.
---

# Manage GTK lifetimes

Review is read-only unless fixes are requested.

- Route ownership and teardown contracts through
  [GTK lifetime and wiring](../../../doc/development/gtk-lifetime.md)
  ([signal connections](../../../doc/development/gtk-lifetime.md#signal-connections),
  [action maps](../../../doc/development/gtk-lifetime.md#action-maps-and-attached-groups),
  [replaceable dependencies](../../../doc/development/gtk-lifetime.md#replaceable-dependencies),
  [view generations](../../../doc/development/gtk-lifetime.md#view-generations),
  [transient attachments](../../../doc/development/gtk-lifetime.md#transient-attachments),
  [cross-lifetime data](../../../doc/development/gtk-lifetime.md#cross-lifetime-data)),
  [GTK dialog lifecycle](../../../doc/system/frontend/gtk/dialog-lifecycle.md),
  and [UIModel and GTK testing](../../../doc/development/test/uimodel-and-gtk.md).
- LLM review calibration: do not flag a synchronous or owner-bounded raw `this`
  capture without proving emitter outliving or teardown reentrancy; conversely,
  flag dereferencing `this` after `co_await` instead of invoking a
  pre-suspension `MainContextCallbackScope` closure, or using
  `sigc::scoped_connection` alone without outer rebinding on replaceable GTK
  properties.
