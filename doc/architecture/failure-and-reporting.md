---
id: architecture.failure-reporting
type: architecture
status: current
domain: system
summary: Defines failure classification, propagation, recovery, reporting, and presentation ownership across Aobus layers.
---
# Failure and reporting architecture

## Scope

This document owns the cross-cutting path from a failure origin to classification, propagation, recovery, reporting, and final application presentation.
It distinguishes domain outcomes, recoverable errors, asynchronous failure events, cancellation control flow, invariant faults, notifications, and diagnostic logging so that no one mechanism becomes a universal error bus.

It does not enumerate `Error::Code`, notification fields, CLI exit values, playback failure kinds, exact retry or fallback behavior, user-visible strings, or logging configuration.
Those exact surfaces and behavioral rules belong in reference and subsystem specifications.
The [outcome channel specification](../spec/failure/outcome-channel.md) owns terminal asynchronous diagnostic behavior and ordering.

## System context

Failure and reporting follow the layer direction defined by the [system architecture](system-overview.md):

```text
external input / device / persisted state / internal invariant
                         |
                         v
core subsystem classifies the outcome
  | value | Result<T> | typed event | exception / contract
                         |
                         v
runtime command or domain service
  | propagate / translate | recover | publish state or event | notify
                         |
             +-----------+-----------+
             |                       |
             v                       v
     UIModel projection       CLI command adapter
             |                       |
             v                       v
          GTK / TUI             stderr + exit status

diagnostic logging receives context at boundary and leaf catch sites;
it is not the application state or recovery path
```

The channels are deliberately distinct.
A successful absence or no-op is a domain value; a rejected synchronous operation returns a recoverable result; a failure after asynchronous acceptance may require a typed event; cancellation unwinds lifetime-bound work; an invariant fault uses an exception or contract; and a user-facing report is an explicit application decision after classification.

The principal public code boundaries are:

| Concern | System layer | Public boundary | Implementation |
|---|---|---|---|
| Recoverable error value | Core libraries | `include/ao/Error.h` | Header-defined `Error` and `Result<T>` value types |
| Invariant exception and contracts | Core libraries | `include/ao/Exception.h` and `gsl-lite` contracts | Core and subsystem call sites |
| Cancellation control flow | Core async library | `include/ao/async/OperationCancelled.h` and `LifetimeScope.h` | `lib/async/` |
| Unobserved coroutine diagnostics | Core async library with application adapter | `include/ao/async/AsyncExceptionHandler.h` and `Runtime.h` | `lib/async/Runtime.cpp` and `app/runtime/Log.cpp` |
| Runtime reporting feed | Application runtime | `app/include/ao/rt/NotificationService.h` and `NotificationState.h` | `app/runtime/NotificationService.cpp` |
| Platform-neutral activity projection | UIModel | `app/include/ao/uimodel/status/activity/` | `app/uimodel/status/activity/` |
| Final presentation and leaf catches | Frontends | Frontend-local | `app/linux-gtk/`, `app/tui/`, and `app/cli/` |

## Responsibilities

### Failure origin and classification

The subsystem where an outcome originates owns its initial semantic classification.
External data, devices, storage, user input, formats, and resource limits use recoverable result channels when the caller can report or react.
Ordinary absence, end-of-stream, no-op, and other normal domain states remain values rather than manufactured errors.
Broken internal preconditions and impossible in-memory states use invariant exceptions or contracts because downstream application policy cannot recover them safely.

Core subsystems expose typed errors and values without knowing runtime notifications, UIModel state, widgets, terminal cells, or CLI formatting.
Private subsystem exceptions may simplify local implementation only when the public subsystem boundary translates them back into its declared channel.

### Boundary propagation and translation

Each composition boundary preserves the semantic distinction chosen by the origin.
A runtime operation that already has a `Result` channel propagates recoverable lower errors rather than laundering them into an exception or empty value.
A boundary may add operation context or translate a third-party exception, but it does not infer behavior from message text or discard diagnostic origin information.

Runtime may reclassify a lower outcome only because the enclosing application operation defines a narrower meaning.
For example, a confirmed lookup miss may become an optional value, while a store fault cannot become “not found” merely because the caller has no error UI.
The exact return shapes and error-code meanings remain specification and reference concerns.

### Recovery authority

The runtime service that owns the affected application state owns recovery policy.
It decides whether to retry, skip, retain the last valid state, stop a workflow, fall back, or request user action.
Core mechanisms report evidence; UIModel and frontends do not reconstruct subsystem recovery policy from an error string.

Recovery and reporting are separate decisions.
A service may recover and publish one session-history warning, stop and publish an until-dismissed error, return a rejection directly to the initiating editor, or log a best-effort preference failure without adding it to the notification feed.
There is no process-wide recovery manager.

### Runtime reporting

`CoreRuntime` owns one `NotificationService` for the active runtime composition.
The service owns an executor-confined in-memory feed, typed notification lifetime, and commands that post, update, and dismiss entries.
Each effective command commits one immutable revision snapshot and publishes one canonical update on the callback executor.
Reentrant commands queue later revisions, while observer exceptions are contained after commit and forwarded to the async-runtime diagnostic handler with revision context.
Transient lifetime is authoritative runtime state: cancellable sleeps return to the callback executor, and matching id-plus-generation evidence commits one expiry revision for every consumer.
Session-history and until-dismissed entries do not schedule expiry.
It does not inspect `Result`, catch subsystem exceptions, choose severity, retry operations, or decide which domain failures deserve a user-facing report.

Domain services and application workflow coordinators decide when a semantic outcome becomes a notification and provide the frontend-neutral content.
They also own deduplication and aggregation when reporting every low-level failure would produce noise or misrepresent one higher-level operation.
Typed domain events remain available when consumers need structured recovery or correlation beyond a notification summary.

### UIModel reporting projection

UIModel adapts runtime reporting state into reusable platform-neutral presentation state.
The activity-status feature accepts each canonical feed revision once, combines its immutable snapshot with library-task progress, and selects compact and detail representations.
It owns presentation-local timeout only for retained info or synthetic completion state; it observes runtime-transient expiry rather than starting a competing authoritative timer.
It also owns presentation-local suppression policy.

UIModel does not mutate the failed subsystem, select retry or skip behavior, or turn a locally hidden activity row into dismissal of the authoritative runtime feed unless an explicit command requests that mutation.
Validation attached to an editor may remain local typed view state instead of entering the global notification feed.

### Frontend and CLI sinks

GTK and TUI render runtime/UIModel reporting state and bind user actions to typed commands.
They own widget, popover, status-bar, terminal-panel, and lifecycle presentation, but equivalent cross-frontend recovery policy remains below them.

Frontend workflow boundaries catch unexpected exceptions that escape their owned callback or coroutine boundary.
They log diagnostic detail and may present a generic internal-error message; they do not reinterpret the exception as a recoverable domain error.
Expected cancellation passes through these catches to its lifetime owner and normally produces no error report.
When presentation requires a stop-aware callback hop, the boundary sends the unexpected exception to the diagnostic handler before that hop and sends only the generic user message afterward.
Cancellation may suppress stale presentation but cannot replace the already captured diagnostic.

The CLI bypasses UIModel and the notification feed for command-scoped reporting.
Command code adapts a recoverable `Error` to a CLI-local `CommandError`, while the top-level runner owns stderr formatting and process status.
Unexpected invariant and standard exceptions are formatted separately from user-command rejection.

### Diagnostic logging

Logging is an operational side channel for developers and operators.
Boundary adapters log information that is too diagnostic or platform-specific for user-facing state, including source location and unexpected exception detail.

The core async layer passes the original `std::exception_ptr` and a short context string to an injected handler rather than defining a second normalized fault object.
Interactive composition adapts that handler to the thread-safe application logger; bare runtime and CLI composition may use the runtime's stderr fallback.
The [outcome channel specification](../spec/failure/outcome-channel.md) owns handler selection, cancellation exclusion, terminal bookkeeping order, future single ownership, and fallback containment.

Logging does not acknowledge a command, mutate runtime state, dismiss a notification, or prove that the user saw a failure.
A notification message is likewise not a substitute for the structured error or log context needed to diagnose its origin.

## Boundaries and dependency direction

- Core libraries define values, errors, exceptions, contracts, and typed subsystem observations without depending on runtime reporting types.
- Runtime depends on core failure channels and owns application recovery, typed public events, and the notification feed; it cannot depend on UIModel or frontend presentation.
- UIModel depends on runtime reporting values and commands but cannot become the recovery authority or catch storage/audio exceptions as presentation policy.
- GTK and TUI consume runtime/UIModel state and own platform leaf catches; platform exception types do not cross into runtime public APIs.
- CLI may use a frontend-local exception to unwind command implementation, but that type does not cross into core or runtime.
- Cancellation types belong to the async execution boundary and are not converted into `Error`, notifications, or failure events merely to make them visible.
- Diagnostic logging may be called at several layers, but no layer depends on log output as a control or state channel.
- Exact error, notification, event, command, and exit-status inventories are delegated to reference and specifications rather than duplicated in this architecture.

## Data and control flow

### Synchronous command rejection

```text
frontend or CLI command
  -> runtime/core operation
  -> Result<T> rejection carrying Error
  -> initiating adapter keeps structured failure
  -> editor/dialog/CLI presents command-scoped result
```

A command-scoped failure does not automatically enter `NotificationService`.
The workflow owner chooses whether persistent or cross-view visibility is also required.

### Failure after asynchronous acceptance

```text
accepted command
  -> worker / decoder / backend activity
  -> typed lower failure with correlation evidence
  -> callback executor
  -> runtime state owner applies recovery
  -> state snapshot / typed failure event
  -> optional notification summary
  -> UIModel and frontend observation
```

The original synchronous result cannot describe a failure that happens after acceptance.
The owning service therefore uses revision, identity, token, or generation evidence appropriate to its subsystem before accepting the observation.

### Cancellation

```text
owner teardown or explicit cancel
  -> stop request
  -> async checkpoint raises OperationCancelled
  -> coroutine unwinds without touching retired state
  -> lifetime boundary consumes expected cancellation
```

Cancellation does not become a domain error merely because it uses exception-shaped control flow.
If cancellation itself fails to quiesce an owner, that separate invariant or shutdown failure follows its own channel.

### Unexpected invariant fault

```text
contract or invariant violation
  -> exception / termination boundary
  -> explicit caller, or async completion / application leaf catch
  -> critical diagnostic log or stderr
  -> optional generic internal-error presentation
```

Leaf catches contain the fault and protect toolkit or executor boundaries.
They do not invent domain recovery or silently continue mutation from a partially failed invariant-sensitive operation.

## Structural constraints

- Every outcome has one classification owner, and every recoverable workflow has one recovery owner.
- A `Result` is never treated as absence without inspecting the declared error semantics.
- Message text is for diagnosis or presentation, never a machine-readable switch for recovery, severity, deduplication, or navigation.
- Adding context preserves the underlying code and deepest useful diagnostic origin unless a documented boundary intentionally reclassifies the operation.
- A typed asynchronous failure retains the correlation evidence needed to reject stale observations before recovery or reporting.
- `NotificationService` is a semantic application feed, not a global exception handler, log sink, or automatic adapter for every failed `Result`.
- Notification feed commands and subscriptions stay on the callback executor; workers and backend callbacks first return through their owning runtime service.
- One committed notification revision has one immutable canonical update, and an observer fault cannot become command failure after that commit.
- Notifications describe user-relevant application outcomes; logs preserve diagnostic detail; neither substitutes for the other.
- One higher-level operation reports once at the owner-selected granularity instead of allowing every lower layer to post independently.
- UI-local validation and fallback state remain local unless the outcome must survive editor dismissal or be visible across application surfaces.
- Runtime reporting values are frontend-neutral and contain no widget, terminal, or platform exception object.
- Expected cancellation is silent at generic exception-reporting leaves and cannot be swallowed before the lifetime boundary has made captured state safe.
- A future-returning task has one explicit exception owner and is not also diagnosed as an unobserved coroutine.
- An injected async exception handler is diagnostic-only, may run concurrently, and cannot mutate executor-affine application state.
- A catch-all at a composition leaf may contain and log an unexpected fault but cannot convert it into an ordinary success value.

## Failure, cancellation, and lifetime boundaries

The [runtime execution architecture](runtime-execution.md) owns executor switching and teardown order.
Failure observations produced on workers, decoder threads, backend threads, or device callbacks become runtime state only after the owning runtime service accepts them on the callback executor.

`NotificationService`, its synchronous canonical update stream, and all feed reads and commands participate in the runtime callback-domain composition; workers do not post directly as a substitute for returning through their service owner.
UIModel subscriptions and frontend views release before the runtime notification owner is destroyed.

Lifetime-bound workflows use stop tokens and `LifetimeScope` so owner teardown cancels outstanding work.
Cancellation is consumed only after the coroutine has unwound and can no longer access retired owner state.
Unhandled coroutine faults are diagnosed at async completion boundaries; expected cancellation is excluded from that reporting.
The application logging adapter remains alive until runtime worker completion handlers have quiesced.

Frontend leaf catches remain alive for the callback source they protect.
During shutdown, final persistence and subsystem quiescence run while their reporting and logging dependencies still exist, but no late worker or audio callback may post into a destroyed runtime feed.

## Implementation map

- [`Error`](../../include/ao/Error.h) and [`Exception`](../../include/ao/Exception.h) define the shared recoverable value and invariant exception foundations.
- [`StorageResult`](../../app/include/ao/rt/StorageResult.h) is a focused runtime example of narrowing storage outcomes without losing diagnostic origin.
- [`OperationCancelled`](../../include/ao/async/OperationCancelled.h), [`LifetimeScope`](../../include/ao/async/LifetimeScope.h), and their implementations under [`lib/async/`](../../lib/async) define cancellation and lifetime completion boundaries.
- [`AsyncExceptionHandler`](../../include/ao/async/AsyncExceptionHandler.h) and [`Runtime.cpp`](../../lib/async/Runtime.cpp) define unobserved coroutine diagnostic ownership without replacing Asio exception transport.
- [`NotificationState`](../../app/include/ao/rt/NotificationState.h), [`NotificationService`](../../app/include/ao/rt/NotificationService.h), and [`NotificationService.cpp`](../../app/runtime/NotificationService.cpp) define the runtime reporting feed.
- [`CoreRuntime.cpp`](../../app/runtime/CoreRuntime.cpp) composes the notification owner with library, source, completion, async, and diagnostic collaborators.
- [`PlaybackFailure`](../../app/include/ao/rt/PlaybackFailure.h), [`PlaybackService.cpp`](../../app/runtime/PlaybackService.cpp), and [`PlaybackSequenceService.cpp`](../../app/runtime/PlaybackSequenceService.cpp) are the principal typed asynchronous failure, recovery, and summary-reporting path.
- [`ActivityStatusViewModel`](../../app/include/ao/uimodel/status/activity/ActivityStatusViewModel.h) and [`ActivityStatusFeedProjection`](../../app/uimodel/status/activity/ActivityStatusFeedProjection.cpp) define the platform-neutral reporting projection.
- GTK [`UiWorkflow`](../../app/linux-gtk/common/UiWorkflow.h), TUI [`Executor.cpp`](../../app/tui/Executor.cpp), and CLI [`CommandError`](../../app/cli/CommandError.h) plus [`Run.cpp`](../../app/cli/Run.cpp) define representative application-leaf containment and presentation boundaries.
- [`Log.h`](../../app/include/ao/rt/Log.h) exposes the application logging surface used by runtime and frontend boundary adapters.
- [`Log.cpp`](../../app/runtime/Log.cpp) supplies the application adapter for injected async exceptions.

## Test map

- [`ErrorTest.cpp`](../../test/unit/core/ErrorTest.cpp), [`ExceptionTest.cpp`](../../test/unit/core/ExceptionTest.cpp), and subsystem error tests under [`test/unit/`](../../test/unit) protect shared values, source locations, and translation boundaries.
- [`AsyncRuntimeTest.cpp`](../../test/unit/runtime/AsyncRuntimeTest.cpp) and [`LifetimeScopeTest.cpp`](../../test/unit/runtime/LifetimeScopeTest.cpp) protect cancellation, executor return, single-owner exception completion, injected diagnostics, and owner lifetime.
- [`UiWorkflowTest.cpp`](../../test/unit/linux-gtk/common/UiWorkflowTest.cpp) protects diagnostic-before-presentation ordering when cancellation wins the callback hop.
- [`LogTest.cpp`](../../test/unit/runtime/LogTest.cpp) protects the retained application-log adapter.
- [`NotificationServiceTest.cpp`](../../test/unit/runtime/NotificationServiceTest.cpp) protects feed mutation, immutable revisions, executor-owned observation, reentrant publication, and observer-fault containment.
- [`NotificationServiceExpiryTest.cpp`](../../test/unit/runtime/NotificationServiceExpiryTest.cpp) protects transient scheduling, callback-executor expiry, generation races, cancellation, and teardown.
- [`PlaybackServiceTest.cpp`](../../test/unit/runtime/PlaybackServiceTest.cpp) and [`PlaybackSequenceServiceTest.cpp`](../../test/unit/runtime/PlaybackSequenceServiceTest.cpp) protect typed failure correlation, recovery ownership, and notification aggregation.
- Activity-status tests under [`test/unit/uimodel/status/activity/`](../../test/unit/uimodel/status/activity) protect the runtime-feed to UIModel boundary and presentation-local suppression.
- [`ActivityStatusWidgetTest.cpp`](../../test/unit/linux-gtk/status/ActivityStatusWidgetTest.cpp) protects GTK rendering, and [`CommandErrorTest.cpp`](../../test/unit/cli/CommandErrorTest.cpp) protects the CLI command adapter.

## Related documents

- [System architecture](system-overview.md)
- [Runtime execution architecture](runtime-execution.md)
- [Presentation architecture](presentation.md)
- [Library architecture](library.md)
- [Playback architecture](playback.md)
- [Workspace architecture](workspace.md)
- [Interactive session lifecycle architecture](interactive-session-lifecycle.md)
- [Persistence and managed-state architecture](persistence-and-managed-state.md)
- [Outcome channel specification](../spec/failure/outcome-channel.md)
- [Error value reference](../reference/failure/error.md)
- [Notification feed specification](../spec/reporting/notification-feed.md) and [model reference](../reference/reporting/notification.md)
- [Activity-status specification](../spec/presentation/activity-status.md) and [surface reference](../reference/presentation/activity-status.md)
- [Decoder session specification](../spec/playback/decoder-session.md) and [decoder error reference](../reference/playback/decoder-error.md) for the remaining core-audio translation boundary
