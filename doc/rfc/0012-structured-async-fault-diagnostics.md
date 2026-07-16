---
id: rfc.0012.structured-async-fault-diagnostics
type: rfc
status: implemented
domain: async
summary: Introduced one injected exception handler for unobserved coroutine failures without replacing Asio exception transport.
depends-on: none
---
# RFC 0012: Injected asynchronous exception diagnostics

## Disposition

Implemented on 2026-07-14 with a narrower design than the original structured-fault proposal.

Boost.Asio already captures exceptions escaping `awaitable` coroutines and gives the terminal `co_spawn` completion handler a `std::exception_ptr`.
The implemented boundary preserves that mechanism and adds only an injectable `AsyncExceptionHandler` that receives the original exception pointer and a short static context string.

The [outcome channel specification](../spec/failure/outcome-channel.md) owns the terminal diagnostic behavior and ordering invariants.
The [runtime execution architecture](../architecture/runtime-execution.md) owns completion, concurrency, and teardown placement, while the [failure and reporting architecture](../architecture/failure-and-reporting.md) owns diagnostic versus user-facing reporting responsibilities.
Those current authorities supersede this proposal; this RFC records why the larger normalized fault model was not adopted.

## Problem

`Runtime::spawnLogged`, `spawnCancellable`, and `LifetimeScope` already received terminal coroutine exceptions from Asio, but formatted them directly to process `stderr`.
GTK and TUI initialize retained application logs, so an unexpected worker failure could be absent from the only useful production diagnostic file.
The hard-coded formatter also prevented tests from recording the terminal exception deterministically.

A second loss path existed in frontend workflows.
Some code caught an unexpected exception, stored its `exception_ptr`, and then crossed a stop-aware callback-executor hop before reporting it.
If owner cancellation won that hop, `OperationCancelled` became the exception escaping the coroutine and the earlier unexpected exception was never diagnosed.

## Dependencies

- Hard: None.
- Conditional: None.
- Integration: [RFC 0013](0013-coherent-application-reporting-policy.md).

RFC 0013 may use the same distinction between diagnostic-only unexpected exceptions and explicitly owned user-facing reports.
It does not change this RFC's exception transport or handler lifetime.

## Goals

- Keep Boost.Asio and `std::exception_ptr` as the only coroutine exception transport.
- Route unobserved root, cancellable, and lifetime-bound failures through one injectable handler.
- Keep expected cancellation silent.
- Preserve future-returning task ownership so `future::get()` rethrows without duplicate reporting.
- Let application composition adapt unexpected exceptions to its retained logger without adding an application dependency to `ao_async`.
- Diagnose an unexpected workflow failure before a later cancellable presentation hop can erase it.
- Keep the diagnostic handler alive until worker completions have quiesced.

## Non-goals

- Define a normalized `AsyncFault` value, task taxonomy, phase enum, correlation id, or operation-label registry.
- Replace `std::exception_ptr`, Asio completion handlers, or normal C++ exception propagation.
- Add a custom signal-safe or allocation-free emergency writer.
- Convert unexpected exceptions into recoverable `Error` values or automatic user notifications.
- Unify every GTK, TUI, CLI, or third-party callback catch in this change.
- Define logging retention, formatting, or transport as a core-async responsibility.

## Proposed design

### Terminal coroutine ownership

`co_spawn` remains the terminal exception boundary for unobserved runtime tasks.
Its completion handler receives `std::exception_ptr`; runtime handling then follows this order:

1. return when there is no exception;
2. recognize and silently consume expected cancellation;
3. invoke the injected handler with the original exception pointer and a short context string;
4. if no handler exists or it throws, use the existing stderr diagnostic as a non-throwing fallback.

`spawnLogged`, `spawnCancellable`, and `spawnWithLifetime` retain their original call shapes.
They use fixed contexts for root, cancellable, and lifetime-bound completion, so callers do not acquire a new task-registration API.

`Runtime::spawn(Task<T>)` returns a future owned by its explicit caller, so no diagnostic handler also reports the same exception.
Void tasks use `boost::asio::use_future` directly; value tasks cross the Asio completion boundary as `std::optional<T>` so Asio never has to default-construct a domain result on the exception path.

### Injection and application logging

The core surface is equivalent to:

```cpp
using AsyncExceptionHandler =
  std::function<void(std::exception_ptr, std::string_view context)>;
```

The handler may be called concurrently from runtime worker threads.
The application logger adapter therefore captures a stable thread-safe logger handle rather than consulting mutable global logger state during a completion callback.

GTK and TUI composition create this adapter after logging initialization and inject it through `AppRuntimeDependencies` into `CoreRuntime` and `async::Runtime`.
Logging remains alive until the runtime has stopped and joined its worker producers.
Bare runtime and CLI compositions may omit the handler and retain stderr fallback behavior.

### Fault-before-presentation ordering

A frontend workflow may need both an operator diagnostic and a generic user-facing failure notice.
When its owner can be cancelled, the workflow reports the caught unexpected exception immediately through `Runtime::reportUnhandledException` and only then awaits the stop-aware callback hop used for presentation.

The diagnostic and presentation remain separate channels:

- the async handler receives exception detail on the thread that owns the catch;
- the callback executor receives only the generic presentation action;
- cancellation after diagnosis may suppress presentation safely, but cannot erase the diagnostic; and
- the coroutine consumes the original exception after reporting, so lifetime completion does not report it again.

Runtime library tasks that previously carried an exception over their own callback hop now queue only callback-affine cleanup and rethrow the original exception before that cancellable hop.
The owning UI workflow then applies the ordering above.

## Alternatives

### Rely only on coroutine propagation

This is sufficient for awaited tasks and futures, but not for fire-and-forget roots whose completion handler is the final owner.
It also cannot recover an earlier exception that application code deliberately caught before a later cancellation exception escaped.

### Normalize every exception into a structured fault value

Rejected as premature.
It duplicates information already retained by `std::exception_ptr`, adds ownership and allocation policy, forces task-label registration through every spawn call, and stabilizes fields with no current consumer beyond logging and tests.
A future tracing or correlation requirement can propose a value model from measured consumers rather than embedding it in basic coroutine completion.

### Call `APP_LOG_ERROR` from `lib/async`

Rejected because it reverses the core-to-application dependency direction and makes the core async library depend on application logging lifetime.

### Keep direct stderr as the routine path

Rejected for interactive composition because GUI launches may not retain a useful terminal stream and tests cannot inject an observer.
Stderr remains only the existing fallback when the application handler is absent or fails.

### Introduce a custom emergency writer

Rejected from this scope because no demonstrated failure requires a platform-specific writer, recursion gate, signal masking, or bounded write loop.
Such a facility requires a separate failure model and native-platform evidence.

## Compatibility and migration

The change has no persisted, command, schema, or user-data compatibility effect.
Existing spawn call sites retain their signatures.

Interactive composition now injects the logging adapter.
The shared GTK UI workflow passes a static diagnostic context and no longer hands `exception_ptr` to its presentation callback.
Internal-error presentation remains generic and executor-affine, while detailed logging moves to the shared handler.

## Validation

- Runtime tests prove root and cancellable failures reach the injected handler with their original exception type.
- Lifetime tests prove scope-owned task failures reach the same handler after task bookkeeping retires.
- A future-returning task proves its exception is rethrown to its caller and is not also reported.
- Cancellation tests prove expected stop completion produces no handler call.
- A deterministic manual-executor regression proves cancellation after fault capture cannot erase the diagnostic or access the retired UI owner.
- Logger tests prove the application adapter writes the supplied exception and context into the retained application log.
- Concurrency and ThreadSanitizer gates validate concurrent completion and teardown behavior.

## Open questions

None for the implemented boundary.
Structured fields, registered task labels, correlation, callback-executor catch convergence, and a stronger emergency transport require separate evidence-backed proposals.

## Promotion plan

The implemented current behavior is owned by:

- [Outcome channel specification](../spec/failure/outcome-channel.md)
- [Runtime execution architecture](../architecture/runtime-execution.md)
- [Failure and reporting architecture](../architecture/failure-and-reporting.md)

No dedicated reference was created because the change introduces no serialized format, command surface, or externally consumed field inventory.
The existing outcome-channel specification owns the exact terminal behavior, while its implementation and test maps lock the narrow C++ seam.
