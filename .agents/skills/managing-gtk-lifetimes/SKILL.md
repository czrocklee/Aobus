---
name: managing-gtk-lifetimes
description: Review or change Aobus GTK signal ownership, dependency rebinding, transient attachments, and widget teardown.
---

# Manage GTK lifetimes

Review is read-only unless fixes are requested. Use the affected sections of
`doc/development/gtk-lifetime.md`: signal connections, replaceable dependencies,
view generations, transient attachments, or cross-lifetime data.

Establish the emitter, receiver, owner, parent, and replacement/teardown graph.
A raw `this` capture alone is not a defect; prove whether the emitter can outlive
the receiver. Blanket scoped connections or weak pointers do not establish that
contract.

For GTK-specific regressions, use `doc/development/test/uimodel-and-gtk.md`.
Completion and evidence reuse follow
`doc/development/test/validation-and-review.md`.
