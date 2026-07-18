---
name: managing-gtk-lifetimes
description: Reviews and changes Aobus GTK ownership, signal wiring, replaceable-source rebinding, widget generations, transient parenting, and delayed callback lifetimes. Use when modifying linux-gtk connections, popovers, models, adjustments, rebuild paths, or teardown.
---

# Managing GTK lifetimes

Use lifetime-aligned wiring for every GTK connection and attachment.

## Required reading

Read `doc/development/gtk-lifetime.md` completely before acting.
When tests change, also read `doc/development/test/uimodel-and-gtk.md` and `doc/development/test/validation-and-review.md`.

## Workflow

1. Trace the emitter, receiver, C++ owner, GTK parent, and every replacement or teardown path.
2. Classify objects as stable hosts, disposable view generations, or transient attachment sessions.
3. Keep owner-level signal connections scoped to the shorter endpoint lifetime.
4. For replaceable dependencies, observe the owning property or slot and reconnect an inner signal scope to the current object.
5. For `set_parent()`, implement one symmetric, idempotent detach path covering close, replacement, cancellation, and destruction.
6. For a simple one-shot popover, use `PopoverAttachment`; keep stable or multi-stage popovers in their owning controller.
7. Defer any operation that would destroy its currently dispatching widget, gesture, action, or view generation, and bind that deferred callback to its owner.
8. Keep generation-local widgets and pointers out of longer-lived controllers; communicate across generations with stable ids, values, commands, or host signals.
9. Add a GTK regression that performs replacement or teardown, then run `./ao check`.

Do not treat blanket `scoped_connection`, weak pointers, refresh calls, or a global wiring registry as substitutes for a proven ownership graph.
Do not report a raw `this` capture as a bug until emitter ownership and destruction order show it can outlive the receiver.
