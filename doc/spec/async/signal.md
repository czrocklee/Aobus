---
id: async.signal
type: spec
status: current
domain: async
summary: Defines owner-affine signal connection, synchronous delivery, reentrancy, fatal observer containment, and deferred lifetime behavior.
---
# Signal delivery

## Scope

This specification owns the observable behavior of `ao::async::Signal` and the lifetime semantics of `ao::async::Subscription`.
It defines connection order, reentrant mutation, the handler failure contract of both primitives, deferred executor delivery, and owner destruction.

It does not define the event payloads, executor choice, reporting transactions, recovery policy, or frontend presentation owned by services that use the primitive.

## Code boundary

The [system architecture](../../architecture/system-overview.md) places reusable signal and subscription mechanisms in the core `ao_async` library.
The [runtime execution architecture](../../architecture/runtime-execution.md) owns callback-executor affinity and requires each service to choose and enforce its serialized observation domain.

The public surface is [`include/ao/async/Signal.h`](../../../include/ao/async/Signal.h) and [`include/ao/async/Subscription.h`](../../../include/ao/async/Subscription.h).
`ao_async` publicly depends on `ao_utility` because `Subscription` is the async vocabulary for [`utility::ScopedRegistration`](../../../include/ao/utility/ScopedRegistration.h).
Application runtime, UIModel, and frontend code may depend on these core types; the core primitive never depends on application events or services.

## Terminology

- The **owner thread** is the thread that constructs the signal; a service normally constructs it on its serialized executor.
- A **slot** is one connected handler and its connection state.
- An **emission** is one synchronous `emit` traversal over the slots eligible when that traversal begins.
- A **posted emission** is a decayed payload deferred through an `async::Executor` before synchronous signal delivery begins.

## Invariants

- `Signal` is unsynchronized.
  `connect`, `emit`, synchronous disconnection, `disconnectAll`, inspection, and destruction occur on the construction thread.
- A service cannot use a mutex or `Signal::post` to avoid defining its own executor-affinity contract.
- Connected handlers run in connection order.
- One emission snapshots the slot count at its start.
  A handler connected during that emission is first eligible for a later emission.
- Disconnecting a slot before its turn prevents that slot from running.
- Disconnecting the active slot keeps its callable alive until the outermost active emission returns.
- Nested emission is permitted.
  Disconnected slots are compacted only after the outermost emission returns.
- `disconnectAll` prevents every not-yet-run slot in the active emission and every later emission from running until new handlers connect.
- Destroying a signal invalidates every outstanding subscription and every posted emission without dereferencing the destroyed signal owner.
- `hasConnectedHandlers` is an owner-domain observation only; it is not a cross-thread synchronization primitive.

## Commands and transitions

`connect(handler)` appends one slot and returns an owning `Subscription`.
Destroying, resetting, or replacing that subscription disconnects its slot.
Moving a subscription transfers that responsibility, and move assignment first disconnects the registration previously owned by the destination.

`emit(args...)` synchronously visits every still-connected slot that was present when the emission began.
Mutations made by a handler affect any later eligible turn according to the invariants above.

`post(executor, args...)` decays and owns its payload, then uses `Executor::defer` to schedule a later turn.
The queued callback retains a weak reference to signal state and starts a normal synchronous emission only when that state is still active.
A supplied executor must run that callback on the signal's construction thread.
A post requested by a handler cannot run inside that handler and remains subject to the supplied executor's turn semantics.

`disconnectAll()` tombstones all current slots.
`hasConnectedHandlers()` reports whether at least one current slot remains connected on the owner domain.

## Failure and cancellation

`Signal` handlers are ordinary callables; the owning emission boundary provides the non-throwing external guarantee.
A signal notifies observers of something the owner has already committed, so a failure an observer reports carries no decision the owner could act on: the state change cannot be undone.
A handler that lets an exception escape therefore enters the AO exception-aware fatal backend at the emission call site instead of relying on raw language termination or stepping over a diverged observer.
Handlers that call fallible operations contain the failure themselves, where enough context remains to choose a meaningful fallback.
Contract-fulfilling handlers are visited in connection order; after a handler escapes, the process aborts immediately and no later-handler guarantee applies.

`emit` and the deferred callback installed by `post` catch at their owning
boundary and invoke `AO_FATAL_EXCEPTION()` with signal context.
Allocation failure while constructing or admitting a posted emission follows the enclosing executor call's exception contract; once accepted, an escaping callback or payload fault is fatal at the deferred emission boundary.
Calling an owner-affine operation, including subscription disconnection or a posted callback's eventual emission, from another thread violates the primitive contract and enters the AO fatal backend before touching slot state.

Signals are therefore for notification only.
A mandatory single-consumer replication phase is a different topology, not an exception protocol; [library change
publication](../library/runtime/change-publication.md) uses a named ordinary replica callable inside its own
non-throwing delivery boundary.

The primitive has no cancellation state.
Destroying a signal before a posted callback runs turns that callback into a successful no-op.

## Implementation map

- [`Signal.h`](../../../include/ao/async/Signal.h) implements the ordinary handler type, shared weak state, slot tombstoning, nested-emission depth, deferred payload ownership, and the `noexcept` owning emission boundary.
- [`Subscription.h`](../../../include/ao/async/Subscription.h) defines the shared async lifetime name over `utility::ScopedRegistration`.
- [`ScopedRegistration.h`](../../../include/ao/utility/ScopedRegistration.h) implements reset, destruction, and move-assignment release behavior.
- [`lib/async/CMakeLists.txt`](../../../lib/async/CMakeLists.txt) records the public `ao_async -> ao_utility` dependency.
- [`ArchitectureAudit.cmake`](../../../app/cmake/ArchitectureAudit.cmake) prevents generic signal primitives from returning to application-owned headers and namespaces.

## Test map

- [`SignalTest.cpp`](../../../test/unit/async/SignalTest.cpp) covers connection order, move-only handlers, connect/disconnect during emission, nested emission, self-disconnection lifetime, `disconnectAll`, subscription moves, owner destruction, decayed posts, later turns, and destroyed-owner posts.
- Fatal subprocess coverage protects off-owner connect, disconnect, and emission diagnostics plus escaping observer diagnostics and abort behavior.
- [`NotificationServiceTest.cpp`](../../../test/unit/runtime/NotificationServiceTest.cpp) proves that the reporting owner adds immutable reentrant-update queuing above the generic primitive.

## Related documents

- [System architecture](../../architecture/system-overview.md)
- [Runtime execution architecture](../../architecture/runtime-execution.md)
- [Failure and reporting architecture](../../architecture/failure-and-reporting.md)
- [Notification feed specification](../reporting/notification-feed.md)
