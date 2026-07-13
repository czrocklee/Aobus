---
id: rfc.0012.structured-async-fault-diagnostics
type: rfc
status: draft
domain: async
summary: Proposes one injected structured diagnostic sink for unhandled asynchronous and lifetime-bound faults.
depends-on: none
---
# RFC 0012: Structured asynchronous fault diagnostics

## Problem

The core asynchronous runtime correctly distinguishes expected cancellation from unexpected coroutine failure, but its unhandled-fault path bypasses the application's diagnostic system.

[`Runtime::spawnLogged`](../../lib/async/Runtime.cpp), `spawnCancellable`, and [`LifetimeScope`](../../lib/async/LifetimeScope.cpp) catch completion exceptions and print directly to process `stderr` with `std::println`.
The current test silences the file descriptor and verifies only that the logging variant does not crash.

This creates several production limitations:

- GTK and TUI initialize rotating structured logs, but root coroutine faults can exist only in an unstructured terminal stream.
- GUI launches may have no useful terminal attached, so the only retained evidence can be lost.
- Context is a fixed string such as `root`, `cancellable`, or `lifetime-bound`; there is no task label, source location, owner, correlation identity, or executor phase.
- `ao::Exception` source location is discarded even though it is available.
- runtime, lifetime-scope, GTK executor, TUI executor, and frontend leaf catches use different text and severity conventions;
- tests cannot inject a recorder and assert the structured diagnostic contract deterministically.

The core async layer cannot solve this by directly depending on `ao::rt::Log`, because core libraries do not depend on application runtime.
The missing boundary is an injected diagnostic sink, not a new application dependency or a user-notification mechanism.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: [RFC 0013](0013-coherent-application-reporting-policy.md).

RFC 0013 distinguishes diagnostic-only invariant faults from user-facing reports.
If both proposals are implemented, the structured sink remains a diagnostic channel and does not become an automatic notification adapter.

## Goals

- Route every unhandled root, cancellable, and lifetime-bound coroutine fault through one injected structured diagnostic contract.
- Preserve expected cancellation as silent control flow.
- Preserve exception category, message, `ao::Exception` source location, task context, and completion domain when available.
- Keep the core async library independent of runtime logging and notification types.
- Let application composition adapt diagnostics to rotating logs, tests, CLI stderr, or another operator sink.
- Make handler failure and pre-initialization behavior explicit and fail-safe.
- Align GTK/TUI executor and frontend leaf diagnostics with the same structure where their boundary semantics match.

## Non-goals

- Convert unexpected exceptions into recoverable `Error` values.
- Retry, resume, or otherwise recover a failed coroutine automatically.
- Post a user notification for every invariant fault.
- Replace domain-specific typed failure events or command results.
- Standardize complete logging configuration, file locations, retention, or formatting.
- Capture native crashes, termination, signals, or memory faults outside C++ exception completion.

## Proposed design

### Core diagnostic value

The async core defines a small frontend-neutral value and sink interface equivalent to:

```text
AsyncFault
  task class: root | cancellable | lifetime-bound
  task label
  exception category
  message
  optional source location
  optional owner/correlation label
  completion phase

AsyncFaultSink
  report(AsyncFault) noexcept
```

The value does not contain `NotificationRequest`, spdlog types, widgets, terminal handles, or application recovery commands.
An unknown non-standard exception retains an explicit category even when no message is available.

`ao::Exception` contributes its original file, line, function when available, and message.
Other `std::exception` values contribute their dynamic category name when portable and their message.

### Injection and task context

`async::Runtime` receives a sink or reporter callback from its composition root.
A default emergency reporter remains available for early startup and tests that intentionally construct a bare runtime.

Spawn APIs accept an optional task context containing a stable operation label and source location captured at the spawn call.
`spawnLogged`, `spawnCancellable`, and `spawnWithLifetime` preserve this context through completion.
Callers use semantic labels such as `library.scan`, `playback.prepare`, or `gtk.import`, not dynamically assembled user data.

`LifetimeScope` no longer owns a separate `stderr` formatter.
Its completion path reports through the runtime that started the task after task bookkeeping is complete.

### Cancellation and completion rules

The completion adapter follows one order:

1. retire task/lifetime bookkeeping;
2. return when there is no exception;
3. recognize and silently consume `OperationCancelled` only at the owning completion boundary;
4. translate every other exception into `AsyncFault`;
5. invoke the sink exactly once.

The sink is diagnostic and `noexcept`.
If a supplied sink violates that contract or cannot operate because logging is unavailable, the runtime invokes a minimal emergency fallback that is safe during startup/shutdown.
Emergency `stderr` is permitted only as this last-resort fallback and is separately testable; routine async fault reporting no longer writes directly to it.

### Application adapter

`CoreRuntime` supplies an adapter that maps structured async faults to the application logger at critical/error severity with stable fields.
The adapter preserves task label, class, exception category, message, and source evidence in one log event.

Interactive application composition initializes logging before starting runtime tasks and keeps the adapter alive until async workers and lifetime-bound completions are quiescent.
Shutdown resets the sink only after worker join and frontend callback drain.

CLI composition may adapt the same structure to stderr when it intentionally runs without application logging.
That is a frontend sink choice, not a direct dependency in `ao_async`.

### Leaf-boundary convergence

GTK and TUI executors, workflow helpers, and top-level runners keep their distinct containment responsibility.
Where they catch an unexpected callback/task exception rather than a command rejection, they construct the same diagnostic structure or use a shared adapter.

Frontend top-level catches may still print a final fatal message to stderr for launch failures or process termination.
They do not duplicate a routine async fault at several nested leaves; one ownership boundary reports it once.

### Integration with reporting policy

An async fault is diagnostic-only by default.
A domain owner may separately publish a generic user-facing internal-error state when an operation cannot continue, but that decision uses typed operation context and is not performed by the async sink.
The diagnostic event and user report may share a correlation id without becoming the same channel.

## Alternatives

### Call `APP_LOG_ERROR` from `lib/async`

This reverses the core-to-runtime dependency direction and makes the async library unusable without application logging.

### Keep stderr and improve the message

More text does not provide retained GUI diagnostics, injection, structured context, deterministic tests, or lifecycle coordination.

### Convert the exception to `Error`

An exception that escaped a root task is an unexpected fault, not a recoverable result retroactively returned to a caller.
Conversion would erase the channel distinction owned by the failure architecture.

### Post a critical notification

The notification feed is user-facing state and may itself depend on runtime callback execution.
Using it as the core fault sink risks recursion, teardown use-after-destruction, and noisy internal-detail exposure.

### Adopt a complete tracing framework first

A tracing system may later consume the structured sink, but it is not required to stop losing current fault evidence.

## Compatibility and migration

The change is internal C++ API evolution with no persisted format impact.
Bare `Runtime` construction remains possible through an explicit default emergency sink or test helper.

Migration proceeds in stages:

1. Introduce the structured value, sink, task context, and recording tests while keeping current stderr as the default adapter.
2. Inject the application logging adapter from runtime composition.
3. Move `Runtime` and `LifetimeScope` routine completions to the injected sink.
4. Align GTK/TUI executor and workflow fault catches where ownership is equivalent.
5. Restrict direct async stderr output to tested emergency and top-level frontend fallbacks.

Existing task behavior, cancellation, worker/callback switching, and future exception transport do not change.

## Validation

- Recording-sink tests assert one structured event for root, cancellable, and lifetime-bound unexpected faults.
- Cancellation at every completion boundary produces no fault event.
- `ao::Exception` preserves message and original source location.
- Standard and unknown exceptions retain distinct categories.
- Task labels and spawn-site evidence survive worker and callback executor hops.
- A failing injected sink triggers exactly one emergency fallback without recursive failure or termination.
- Application tests prove routine async faults reach the rotating app logger rather than direct stderr.
- Shutdown tests prove the sink outlives all async completions and receives no callback after teardown.
- Existing future-returning task APIs still rethrow to their explicit caller instead of also reporting as unhandled.
- GTK/TUI leaf tests prove one owner reports a fault once.
- The completed implementation passes async concurrency tests, ThreadSanitizer validation, `./ao check`, and the documentation gate.

## Open questions

- Should the core value store `std::exception_ptr` for advanced sinks or only normalized safe fields?
- Which task-context fields are stable enough for tests and operational search?
- Should task labels be free strings, registered identifiers, or compile-time literals?
- What emergency fallback is safe on every supported platform before and after logging lifetime?
- Which GTK/TUI leaf catches should converge immediately and which remain platform-specific top-level policy?

## Promotion plan

If accepted, update the [runtime execution architecture](../architecture/runtime-execution.md) with diagnostic-sink ownership, injection, and teardown order.
Update the [failure and reporting architecture](../architecture/failure-and-reporting.md) to distinguish structured invariant diagnostics from optional user-facing internal-error reports.

Add an async fault specification for completion, cancellation exclusion, and sink failure behavior, plus a reference for exact diagnostic fields and task classes if the public core surface remains stable.
Update development guidance for labeling root tasks, testing fault sinks, and reviewing direct stderr use.
CLI reporting documentation remains separate because command rejection and top-level process status are not async-fault diagnostics; exact CLI behavior moves to reference when that legacy surface migrates.
