---
id: failure.outcome-channel
type: spec
status: current
domain: system
summary: Defines how Aobus represents normal outcomes, recoverable failures, cancellation, and invariant faults across public boundaries.
---
# Outcome channel specification

## Scope

This specification owns the behavioral distinction between normal domain values, recoverable `Result` failures, asynchronous failure observations, cancellation control flow, and invariant faults.
It defines how those channels are preserved or translated at public subsystem and application boundaries.

It delegates the exact `Error` fields and code inventory to the [error value reference](../../reference/failure/error.md), cross-layer ownership to the [failure and reporting architecture](../../architecture/failure-and-reporting.md), executor and cancellation mechanics to the [runtime execution architecture](../../architecture/runtime-execution.md), and subsystem-specific recovery behavior to the owning subsystem specification.

## Code boundary

The top-level layer direction is defined by the [system architecture](../../architecture/system-overview.md), and failure ownership is refined by the [failure and reporting architecture](../../architecture/failure-and-reporting.md).
The shared recoverable and invariant-fault foundations are public core-library types under `include/ao/`; cancellation belongs to `include/ao/async/`; runtime services under `app/include/ao/rt/` preserve or deliberately narrow those channels without depending on UIModel or a frontend.

Core code must not depend on runtime notifications or presentation state to classify an outcome.
UIModel and frontends may adapt an already classified outcome for interaction or display, but they must not recover subsystem state by interpreting an error message.

## Terminology

- A **normal outcome** is a successful domain state such as absence, end of stream, an unchanged command, or an empty result.
- A **recoverable failure** is an expected rejection caused by external input, persisted data, IO, a device, an unsupported capability, or a finite resource condition that a caller can report, skip, retry, or otherwise handle.
- An **asynchronous failure observation** reports a recoverable failure that occurs after a command has already been accepted.
- **Cancellation** is lifetime or command control flow that stops work without classifying the stopped operation as failed.
- An **invariant fault** is a broken internal precondition or impossible in-memory state for which ordinary caller recovery would be unsafe.
- A **translation boundary** is the narrow public edge that converts a private or third-party failure mechanism into the channel promised by the enclosing operation.

## Invariants

- A normal outcome uses the narrowest successful value shape and is not manufactured into an `Error` merely to make it observable.
- A recoverable external failure crosses a public boundary as `Result<T>` or a typed asynchronous observation; it does not escape as an invariant exception.
- An invariant fault may use `ao::Exception` or a contract check and must not be laundered into an ordinary success value or a generic recoverable error.
- Cancellation remains distinct from recoverable failure and invariant fault, even when exception-shaped control flow performs the unwind.
- A boundary that adds context preserves the original error code and deepest useful diagnostic location unless the enclosing operation explicitly documents a semantic reclassification.
- Machine behavior never branches on `Error::message`; codes, typed state, identities, revisions, generations, or tokens carry machine-readable meaning.
- A `Result` failure is never treated as absence without checking the declared code contract; only an explicitly documented `NotFound` collapse may become an absence value.
- A third-party exception is contained at the narrow wrapper boundary and translated to the enclosing operation's declared channel.
- A private subsystem exception used to reduce local propagation boilerplate is caught only as its domain-specific leaf type and never becomes part of the public failure vocabulary.
- Acceptance and completion are distinct: a failure after synchronous acceptance uses a typed event or state observation rather than retroactively changing the accepted command result.

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

## Failure and cancellation

External files, user-authored text, persisted records, IO, devices, unsupported formats or capabilities, and resource exhaustion are recoverable when the public caller can react.
Being off a real-time path does not change this classification.

Public parsing or media boundaries may use a private error-carrying exception internally, but their public `Result` boundary catches only that private leaf.
Unrelated exceptions, including allocation and logic faults, are not converted to domain errors by a broad catch.

Cancellable coroutines propagate `ao::async::OperationCancelled` until the lifetime boundary that owns completion.
A broad catch inside cancellable work must preserve cancellation before handling other exceptions, and expected cancellation does not produce a notification or generic error report by default.

## Frontend observations

An initiating editor or command adapter may keep a synchronous rejection local when the user can correct it in place.
A runtime state owner may additionally publish typed state or a notification when the outcome must outlive the initiating surface or be visible across the application.

UIModel normally converts already-classified runtime values into presentation values.
Explicit user-input parsing may retain a `Result` long enough to produce validation state, while advisory presentation heuristics may map an invalid suggestion input to “no recommendation” when query execution is not being attempted.

## Implementation map

- [`Error.h`](../../../include/ao/Error.h) defines `ao::Error`, `ao::Result<T>`, and `makeError`.
- [`Exception.h`](../../../include/ao/Exception.h) defines `ao::Exception` and source-location-preserving throw helpers.
- [`StorageResult.h`](../../../app/include/ao/rt/StorageResult.h) demonstrates the deliberate `NotFound`-to-absence translation used by runtime storage reads.
- Domain-private translation helpers live under subsystem `detail/` boundaries such as [`DecoderError.h`](../../../include/ao/audio/detail/DecoderError.h), [`LibraryError.h`](../../../include/ao/library/detail/LibraryError.h), [`QueryError.h`](../../../include/ao/query/detail/QueryError.h), and [`MediaError.h`](../../../include/ao/media/detail/MediaError.h).

## Test map

- [`ErrorTest.cpp`](../../../test/unit/core/ErrorTest.cpp) and [`ExceptionTest.cpp`](../../../test/unit/core/ExceptionTest.cpp) protect value, inheritance, and diagnostic-location behavior.
- [`StorageResultTest.cpp`](../../../test/unit/runtime/StorageResultTest.cpp) protects the declared `NotFound` collapse and preservation of other failures.
- Subsystem tests under [`test/unit/audio/`](../../../test/unit/audio), [`test/unit/library/`](../../../test/unit/library), [`test/unit/query/`](../../../test/unit/query), and [`test/unit/runtime/`](../../../test/unit/runtime) protect boundary-specific return and translation behavior.

## Related documents

- [Failure and reporting architecture](../../architecture/failure-and-reporting.md)
- [Error value reference](../../reference/failure/error.md)
- [Runtime execution architecture](../../architecture/runtime-execution.md)
- [Notification feed specification](../reporting/notification-feed.md)
