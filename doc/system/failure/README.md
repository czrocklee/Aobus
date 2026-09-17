---
id: architecture.failure-reporting
---
# Failure and reporting architecture

This page explains who classifies a failure, who may recover, and how an outcome
reaches an application surface. Use the focused contracts for exact behavior:

- [Choose an outcome channel](outcome-channel.md)
- [Recoverable error meanings](error.md)
- [Fatal contract boundaries](fatal.md)
- [Private and foreign exception containment](exception-carriers.md)
- [Notifications and expiry](notification-feed.md)

## One origin, one recovery owner

The subsystem where an outcome originates classifies it. Ordinary absence,
end-of-stream, and no-op states are values. Expected failures from external
input, persisted data, IO, devices, unsupported capabilities, and finite
resources use a recoverable `Result` or a typed asynchronous observation.
Broken preconditions, postconditions, established invariants, and mandatory
infrastructure failures use the AO fatal facility when continuing would be
untruthful.

Core libraries expose those values without knowing about notifications,
UIModel, widgets, terminals, or CLI formatting. A private or third-party
exception may exist only within the adapter or subsystem region that translates
it to the public channel. The [exception-carrier reference](exception-carriers.md)
records the permitted regions.

A composition boundary preserves the origin's distinction. It may add context
or deliberately narrow one documented case, but it must not infer policy from
message text, erase diagnostic origin, or turn an unrelated exception into a
domain error. A `Result` failure is not absence unless that operation explicitly
collapses its `NotFound` case.

The runtime service that owns affected application state owns recovery. It may
retry, skip, keep the last valid state, stop a workflow, or request user action.
Core reports evidence; UIModel and frontends do not reconstruct recovery from an
error sentence. There is no process-wide recovery manager.

## Channel selection

```text
normal outcome              -> value / optional / ordinary state
recoverable command failure -> Result<T>
failure after acceptance    -> typed event or state observation
owner retirement            -> cancellation and unwind
broken internal contract    -> AO fatal entry and abort
user-visible history        -> explicit NotificationService command
operator diagnostics        -> logging side channel
```

Acceptance and completion are distinct. A command that was accepted cannot be
retroactively rejected when later worker, decoder, or device work fails. The
state owner receives a typed observation, rejects stale evidence using a
revision, identity, token, or generation, applies recovery, and only then
chooses whether to report.

Cancellation is control flow, not an `Error`. A lifetime boundary consumes it
only after the coroutine has unwound far enough that retired state is no longer
reachable. Caller-owned futures preserve their original exception. Runtime-owned
fire-and-forget roots complete bookkeeping and send every other escaping
exception to the exception-aware fatal entry; they do not log and continue.

## Reporting is not recovery

`CoreRuntime` owns one callback-executor-confined `NotificationService`. Domain
services decide which semantic outcomes become reports and at what granularity.
The feed stores bounded frontend-neutral entries, supports producer-owned report
keys, expires transient entries, and publishes immutable snapshots. It does not
inspect arbitrary `Result` values, catch subsystem exceptions, choose severity,
retry operations, or become a global error bus.

Recovery and reporting are independent. An editor may keep a correctable
rejection local; a service may recover and add one history warning; a workflow
may stop and pin an error; a best-effort preference failure may be logged only.
One higher-level operation reports once instead of allowing every lower layer to
post independently.

UIModel resolves structured report and progress values through the message
catalog and projects them for shared presentation. It may suppress or time out a
local presentation, but it does not remove authoritative runtime state or mutate
the failed subsystem. GTK, TUI, WinUI, and AppKit render that projection and own
platform leaf adapters. CLI instead maps command-scoped `Error` values to its
local command diagnostic and exit status.

## Observer and asynchronous boundaries

Notification commands, feed reads, subscriptions, and observer callbacks remain
on the callback executor. A worker or device callback first returns through its
runtime owner. One effective feed mutation commits one immutable snapshot.
Reentrant feed commands queue a later update; they do not nest delivery into the
current observer traversal.

A notification observer runs after commit. Its exception cannot roll the change
back, so the owning signal boundary enters AO fatal handling rather than stepping
over a diverged observer. Expected fallible work is contained by the observer
where it still has enough context to choose a fallback.

The same principle applies at executor, coroutine, thread, and platform ABI
roots: finish mandatory queue, task, state, or shutdown bookkeeping, then send
an unexpected escape to `AO_FATAL_EXCEPTION()`. Expected framework or platform
failures are translated only where the owning operation defines a recoverable
fallback.

## Diagnostics and fatal lifetime

Logging is an operational side channel. It does not acknowledge commands,
change application state, prove that a user saw a report, or supply recovery
policy.

The Core fatal backend always emits a bounded emergency record. A non-realtime
entry may then invoke the single registered application sink and always aborts;
the realtime entry bypasses that sink. Sink absence, rejection, recursion,
concurrency, formatting failure, or a throwing sink cannot convert the fault to
recovery.

The logger and registered sink must outlive every possible fatal producer.
Frontends retire callbacks, audio/device owners quiesce, and runtime workers join
before the application unregisters the sink and destroys logging. See
[runtime execution](../execution/README.md) and
[session lifecycle](../session-lifecycle.md) for teardown order.

## Layer boundaries

- Core owns values, `Error`, fatal contracts, narrowly scoped exception
  transports, cancellation primitives, and typed subsystem observations.
- Runtime preserves those channels, owns application recovery and asynchronous
  correlation, and owns the notification feed.
- UIModel adapts classified runtime state into platform-neutral presentation; it
  is not a second recovery authority.
- Frontends own platform leaf catches and final rendering. Platform exception
  types do not cross runtime public APIs.
- Diagnostic logging may be called at several layers, but no layer depends on
  log output as a control or state channel.
- Runtime reporting values contain no widget, terminal, or platform exception
  object, and machine policy never branches on resolved copy.

## Source and test anchors

The shared value and fatal surfaces are
[`Error.h`](../../../include/ao/Error.h) and
[`Contract.h`](../../../include/ao/Contract.h).
`Runtime.cpp` and `LifetimeScope.cpp` own asynchronous terminal observation;
`NotificationService.cpp` owns feed commit and reentrant publication; `Log.cpp`
owns the application fatal-sink adapter.

Focused coverage includes `ErrorTest.cpp`, fatal subprocess probes,
`AsyncRuntimeTest.cpp`, `LifetimeScopeTest.cpp`, `NotificationServiceTest.cpp`,
`NotificationServiceExpiryTest.cpp`, and `LogTest.cpp`. Producing subsystems own
their recovery and correlation tests.
