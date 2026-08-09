---
id: failure.outcome-channel
type: spec
status: current
domain: system
summary: Defines how Aobus represents normal outcomes, recoverable failures, cancellation, and invariant faults across public boundaries.
---
# Outcome channel specification

## Scope

This specification owns the behavioral distinction between normal domain values, recoverable `Result` failures, asynchronous failure observations, cancellation control flow, and fatal contract faults.
It defines how those channels are preserved or translated at public subsystem and application boundaries.
It also owns terminal diagnostics for asynchronous invariant faults that have no explicit caller-owned result channel.

It delegates the exact `Error` fields and code inventory to the [error value reference](../../reference/failure/error.md), cross-layer ownership to the [failure and reporting architecture](../../architecture/failure-and-reporting.md), executor and cancellation mechanics to the [runtime execution architecture](../../architecture/runtime-execution.md), and subsystem-specific recovery behavior to the owning subsystem specification.

## Code boundary

The top-level layer direction is defined by the [system architecture](../../architecture/system-overview.md), and failure ownership is refined by the [failure and reporting architecture](../../architecture/failure-and-reporting.md).
The shared recoverable and fatal-contract foundations are public Core types under `include/ao/`; subsystem-private translation carriers remain under their owning `lib/` implementation when no public consumer requires their declarations; cancellation belongs to `include/ao/async/`; runtime services under `app/include/ao/rt/` preserve or deliberately narrow those channels without depending on UIModel or a frontend.

Core code must not depend on runtime notifications or presentation state to classify an outcome.
UIModel and frontends may adapt an already classified outcome for interaction or display, but they must not recover subsystem state by interpreting an error message.

## Terminology

- A **normal outcome** is a successful domain state such as absence, end of stream, an unchanged command, or an empty result.
- A **recoverable failure** is an expected rejection caused by external input, persisted data, IO, a device, an unsupported capability, or a finite resource condition that a caller can report, skip, retry, or otherwise handle.
- An **asynchronous failure observation** reports a recoverable failure that occurs after a command has already been accepted.
- **Cancellation** is lifetime or command control flow that stops work without classifying the stopped operation as failed.
- An **invariant fault** is a broken internal precondition or impossible in-memory state for which ordinary caller recovery would be unsafe.
- A **precondition fault** means a caller violated an obligation knowable before the call; a **postcondition fault** means a normal return cannot satisfy the callee's guarantee.
- A **fatal infrastructure fault** means mandatory work cannot preserve its contract after recovery has ceased to be truthful.
- An **unobserved asynchronous fault** is an exception that escapes a runtime-owned root, cancellable, or lifetime-bound coroutine whose terminal completion is the last exception owner.
- A **translation boundary** is the narrow public edge that converts a private or third-party failure mechanism into the channel promised by the enclosing operation.

## Invariants

- A normal outcome uses the narrowest successful value shape and is not manufactured into an `Error` merely to make it observable.
- A recoverable external failure crosses a public boundary as `Result<T>` or a typed asynchronous observation; it does not escape as an invariant exception.
- Project fatal checks use the AO facility and must not be laundered into an ordinary success value or a generic recoverable error.
- Production code does not invoke raw gsl-lite contracts; all project-owned runtime contract diagnostics pass through the AO facility.
- Production runtime checks do not use the configuration-dependent C `assert`
  macro; caller, postcondition, internal, and realtime facts use the matching AO
  fatal category. Compile-time `static_assert` remains unaffected.
- Cancellation remains distinct from recoverable failure and invariant fault, even when exception-shaped control flow performs the unwind.
- A boundary that adds context preserves the original error code and deepest useful diagnostic location unless the enclosing operation explicitly documents a semantic reclassification.
- Machine behavior never branches on `Error::message`; codes, typed state, identities, revisions, generations, or tokens carry machine-readable meaning.
- A `Result` failure is never treated as absence without checking the declared code contract; only an explicitly documented `NotFound` collapse may become an absence value.
- A third-party exception is contained at the narrow wrapper boundary and translated to the enclosing operation's declared channel.
- A private subsystem exception used to reduce local propagation boilerplate is caught only as its domain-specific leaf type and never becomes part of the public failure vocabulary.
- Acceptance and completion are distinct: a failure after synchronous acceptance uses a typed event or state observation rather than retroactively changing the accepted command result.
- A future-returning asynchronous task has one explicit caller-owned exception channel and is never also reported as an unobserved asynchronous fault.
- Expected cancellation never enters the exception-aware fatal path.
- Terminal task bookkeeping completes before an unobserved asynchronous fault enters the fatal backend.
- Fatal sink absence, rejection, or failure cannot escape the terminal completion boundary or prevent abort.
- Every AO fatal category emits its emergency diagnostic and ends through `std::abort()`; an application sink is best effort and cannot change that outcome.
- A fatal condition and its diagnostic arguments contain no mutation or query required for correctness.

## Commands and transitions

The successful domain value determines the base return shape before a recoverable channel is added.

| Shape | Contract |
|---|---|
| `bool` | A binary predicate or normal yes/no outcome with no external failure mode. |
| `T*` or `T const*` | A borrowed in-memory lookup where `nullptr` means absent. |
| `std::optional<T>` | A value lookup where absence is the only normal miss state. |
| `Result<>` | A command with no successful payload that can fail recoverably. |
| `Result<T>` | A value-producing operation that can fail recoverably; a diagnostic lookup miss uses `NotFound`. |
| `Result<Enum>` | An operation with several normal success states and an additional recoverable failure channel. |

New APIs do not use `Result<bool>` to overload command success and domain state.
They also avoid `Result<std::optional<T>>` unless absence and failure are independently meaningful and the owning specification states both semantics explicitly.

When runtime composes a lower operation, it follows the enclosing operation's declared intent:

1. A normal miss remains a value only when the public operation defines that miss as ordinary flow.
2. An operation with a `Result` channel propagates every recoverable lower error unless it explicitly and losslessly reclassifies one code.
3. An operation without a recoverable channel may collapse only its documented normal miss; every unexpected lower failure remains an invariant fault and preserves diagnostic origin.
4. A transactional mutation either commits its complete effective change or reports/raises its declared failure; it never reports success after silently committing a successful subset.
5. A root library write body returns `Result<T>` through its transaction owner; any error terminalizes that root before it crosses the boundary, while an unrelated exception is rethrown only after the same rollback.

## Failure and cancellation

### Project fatal checks

Use the fatal form whose owner can know the failed fact:

| Fact owner | Channel |
|---|---|
| Caller-supplied argument, capability, or documented call order | `AO_EXPECTS` |
| Callee's result or observable state guarantee on normal return | `AO_ENSURES` |
| Construction, successful validation, private lifecycle, or internal state transition | `AO_INVARIANT` |
| Mandatory infrastructure after recovery is no longer truthful | `AO_FATAL` |
| Realtime capacity or state-machine planning invariant | `AO_RT_INVARIANT` |

`AO_INVARIANT` states a proposition about internal state that must hold at the check point.
`AO_FATAL` is selected only after control flow has already established an unrecoverable mandatory failure and no useful condition remains to test.
An unconditional terminal branch must use `AO_FATAL` rather than `AO_INVARIANT(false, ...)`.

All five forms are enabled in every build configuration, preserve the failing call site's source location, never return, never throw, and end through one Core abort backend.
The ordinary path emits an emergency record before attempting the registered application sink.
The realtime path uses static context and bypasses dynamic formatting and application logging.
Exact category tokens, macro call shapes, diagnostics, sink lifetime, truncation, and reentrancy behavior belong to the [fatal facility reference](../../reference/failure/fatal.md).

External files, user-authored text, persisted records, IO, devices, unsupported formats or capabilities, and resource exhaustion are recoverable when the public caller can react.
Being off a real-time path does not change this classification.
Library read- and write-transaction construction has no actionable live-runtime branch: native begin failure first unwinds any writer ownership, then fails through `AO_FATAL` rather than returning an operation `Result`.
Revision exhaustion fails through `AO_INVARIANT` before mutation because the maximum valid committed value is a physically unreachable sentinel, not an actionable resource-pressure state.

Public parsing or media boundaries may use a private error-carrying exception internally, but their public `Result` boundary catches only that private leaf.
The library transaction owner follows the same exact-catch rule for its private native mutation marker; runtime and frontend code do not catch that storage type.
The library owner also translates its private recoverable carrier at its own open or root-write boundary.
After a complete open validates a structured store, a later row-integrity failure is an `AO_INVARIANT` fault and a later native read failure is `AO_FATAL`.
Neither is converted into a partial operation result or allowed to unwind into runtime or frontend recovery code.
Unrelated exceptions, including allocation and logic faults, are not converted to domain errors by a broad catch.

Cancellable coroutines propagate `ao::async::OperationCancelled` until the
lifetime boundary that owns completion.
A broad catch inside cancellable work must preserve cancellation before
handling other exceptions.
It either immediately rethrows cancellation or, at an operation-owned terminal
boundary, exhaustively classifies cancellation, completes the operation's
mandatory bookkeeping, and then follows the operation's documented
cancellation channel.
Expected cancellation does not produce a notification or generic error report
by default.

### Terminal asynchronous diagnostics

Boost.Asio terminal completion remains the exception transport for runtime-owned coroutines.
When a root, cancellable, or lifetime-bound coroutine completes, the runtime follows this order:

1. A completion without an exception produces no diagnostic.
2. An exception classified as expected cancellation is consumed without a diagnostic.
3. Any terminal bookkeeping owned by that completion path is retired.
4. Every other exception is passed once, unchanged, to
   `AO_FATAL_EXCEPTION()` with a short context that identifies the owning
   completion boundary.
5. The Core fatal backend emits its emergency diagnostic, attempts the registered application fatal sink, and aborts.

The fatal entry runs synchronously at the terminal boundary and may be reached concurrently by different worker threads.
It does not acknowledge a command, recover domain state, publish a user-facing outcome, or continue the process.

For a `LifetimeScope` task, retirement means marking the task complete and removing it from the scope before fatal entry.
The subprocess test observes that bookkeeping through an independent marker before process death.

`Runtime::spawn` returns a caller-owned `TaskFuture<T>` and retains an escaping task exception there instead of invoking the diagnostic handler.
For non-void tasks, its private future carries `std::optional<T>` so neither Boost.Asio nor the standard-library future needs to default-construct the domain result before completion.
`TaskFuture<T>::get()` returns the produced value or rethrows the original task exception; a successful terminal state without the required value is an invariant fault.

The exception-aware fatal diagnostic distinguishes standard exceptions, whose `what()` detail is available, from unknown exceptions.
Formatting, emergency output, and fatal-sink failure are contained and never replace the final abort.

## Frontend observations

An initiating editor or command adapter may keep a synchronous rejection local when the user can correct it in place.
A runtime state owner may additionally publish typed state or a notification when the outcome must outlive the initiating surface or be visible across the application.

UIModel normally converts already-classified runtime values into presentation values.
Explicit user-input parsing may retain a `Result` long enough to produce validation state, while advisory presentation heuristics may map an invalid suggestion input to “no recommendation” when query execution is not being attempted.

Frontend and platform adapters translate only expected failures for which their owning specification defines a recoverable fallback.
An unexpected exception escapes to the owning coroutine, executor, thread, or ABI root, which finishes mandatory bookkeeping and aborts through the exception-aware fatal entry.
It is not converted into generic presentation followed by continued execution.

## Implementation map

- [`Error.h`](../../../include/ao/Error.h) defines `ao::Error`, `ao::Result<T>`, and `makeError`.
- [`Contract.h`](../../../include/ao/Contract.h) and [`Fatal.cpp`](../../../lib/utility/Fatal.cpp) define project fatal categories, macros, diagnostics, registration, and abort.
- [`TaskFuture.h`](../../../include/ao/async/TaskFuture.h) defines explicit caller-owned result and exception transport without requiring default-constructible task values.
- [`Runtime.h`](../../../include/ao/async/Runtime.h), [`Runtime.cpp`](../../../lib/async/Runtime.cpp), and [`LifetimeScope.cpp`](../../../lib/async/LifetimeScope.cpp) implement terminal ownership, cancellation exclusion, fatal entry, and bookkeeping order.
- The [exception carrier reference](../../reference/failure/exception-carriers.md) owns the exact carrier whitelist and catch owners.
- Domain-private translation helpers live behind subsystem implementation boundaries: [`DecoderError.h`](../../../lib/audio/detail/DecoderError.h), [`LibraryError.h`](../../../lib/library/detail/LibraryError.h), [`MediaError.h`](../../../lib/media/detail/MediaError.h), and [`QueryError.h`](../../../lib/query/detail/QueryError.h).

## Test map

- [`ErrorTest.cpp`](../../../test/unit/core/ErrorTest.cpp) protects recoverable value and diagnostic-location behavior.
- Fatal tests under [`test/unit/core/`](../../../test/unit/core) and the dedicated `ao_fatal_probe` under [`test/fatal/`](../../../test/fatal) protect lazy conditions, registration, emergency fallback, reentrancy, source context, and process death.
- Build-owned source guardrails in [`CMakeLists.txt`](../../../CMakeLists.txt) reject production C assertions, raw gsl-lite contract spellings, unconditional false AO contracts, and production `std::unreachable()` at every normal build.
- [`AsyncRuntimeTest.cpp`](../../../test/unit/runtime/AsyncRuntimeTest.cpp) protects future single ownership, non-default-constructible value transport, and cancellation exclusion.
- [`LifetimeScopeTest.cpp`](../../../test/unit/runtime/LifetimeScopeTest.cpp) plus runtime fatal subprocess scenarios protect task retirement before fatal entry.
- [`LibraryTaskServiceTest.cpp`](../../../test/unit/runtime/library/LibraryTaskServiceTest.cpp) protects callback-affine failure cleanup before exception propagation.
- [`LogTest.cpp`](../../../test/unit/runtime/LogTest.cpp) protects the registered fatal logging adapter.
- Subsystem tests under [`test/unit/audio/`](../../../test/unit/audio), [`test/unit/library/`](../../../test/unit/library), [`test/unit/query/`](../../../test/unit/query), and [`test/unit/runtime/`](../../../test/unit/runtime) protect boundary-specific return and translation behavior.

## Related documents

- [Failure and reporting architecture](../../architecture/failure-and-reporting.md)
- [Error value reference](../../reference/failure/error.md)
- [Fatal facility reference](../../reference/failure/fatal.md)
- [Exception carrier reference](../../reference/failure/exception-carriers.md)
- [Decision 0007](../../decision/0007-unify-fatal-diagnostics-and-abort.md)
- [Runtime execution architecture](../../architecture/runtime-execution.md)
- [Notification feed specification](../reporting/notification-feed.md)
