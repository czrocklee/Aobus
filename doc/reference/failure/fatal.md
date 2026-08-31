---
id: failure.fatal-facility
type: reference
status: current
domain: system
summary: Enumerates the AO fatal categories, macros, diagnostic payload, sink registration surface, and abort behavior.
---
# Fatal facility reference

## Scope and version

This reference enumerates the in-process fatal surface declared by [`Contract.h`](../../../include/ao/Contract.h).
It is a source-level C++ API rather than a serialized or wire format.
The category tokens and emergency marker are diagnostic surfaces, not user-visible recovery codes.

Outcome selection belongs to the [outcome channel specification](../../spec/failure/outcome-channel.md), and ownership belongs to the [failure and reporting architecture](../../architecture/failure-and-reporting.md).

## Code boundary

The [system architecture](../../architecture/system-overview.md) places the facility in the Core utility layer.
The public declaration lives under `include/ao/`, and the backend lives under `lib/utility/`.
Core code may invoke the macros and register no application dependency; the application runtime may depend downward on the registration seam to adapt its logger.

## Surface

### Fatal categories

| `FatalCategory` value | Diagnostic token | Meaning |
|---|---|---|
| `Expects` | `expects` | A caller violated a documented precondition. |
| `Ensures` | `ensures` | A function cannot satisfy its documented normal-return guarantee. |
| `Invariant` | `invariant` | Construction, validation, or an internal transition previously established a false fact. |
| `Fatal` | `fatal` | Mandatory infrastructure cannot preserve its contract and recovery is no longer truthful. |
| `RealtimeInvariant` | `realtime-invariant` | A realtime capacity or state-machine planning invariant failed. |
| `UnhandledException` | `unhandled-exception` | An owning terminal boundary received an escaping non-cancellation exception after mandatory bookkeeping. |

`fatalCategoryName(FatalCategory)` returns the token above without allocation or failure.

### Conditional macros

```cpp
AO_EXPECTS(condition, context...)
AO_ENSURES(condition, context...)
AO_INVARIANT(condition, context...)
AO_RT_INVARIANT(condition, staticContext)
```

Each conditional macro evaluates `condition` exactly once.
Context arguments are evaluated only on failure.
The first three forms accept no context, one `std::string_view`-compatible context, or a compile-time `std::format` string plus arguments.
Formatted context is bounded; truncation is reported in the diagnostic payload rather than allocating an unbounded message.

`AO_RT_INVARIANT` accepts one character-array context and performs no dynamic formatting.
It does not invoke the registered application sink.

### Unconditional macro

```cpp
AO_FATAL(context...)
```

`AO_FATAL` accepts the same ordinary context forms as `AO_EXPECTS` and always enters the fatal backend.
It is the unconditional terminal primitive, not a conditional assertion.
Use `AO_FATAL` after control flow has already established an unrecoverable failure and no meaningful condition remains to express.
Use `AO_INVARIANT` instead when the check states an internal fact that must hold at that point.
Do not encode an unconditional terminal branch as `AO_INVARIANT(false, ...)`.

### Location-forwarding forms

```cpp
AO_EXPECTS_AT(location, condition, context...)
AO_FATAL_AT(location, context...)
AO_RT_FATAL_EXCEPTION_AT(location, staticContext)
```

These forms are reserved for a helper or owner boundary that has already captured its caller's `std::source_location`.
`AO_EXPECTS_AT` and `AO_FATAL_AT` retain the ordinary category, formatting, sink, and abort behavior.
`AO_RT_FATAL_EXCEPTION_AT` emits `UnhandledException` with static context and bypasses the application sink.
Ordinary call sites use the non-`_AT` macros; no production caller invokes the `ao::detail::abortFatal` or `ao::detail::abortRealtime` implementation entry directly.

### Exception-aware entry

```cpp
AO_FATAL_EXCEPTION(exceptionPtr, context)
```

`AO_FATAL_EXCEPTION` accepts one `std::exception_ptr` and a short boundary
context. It preserves the macro call site, distinguishes standard exceptions
from unknown exceptions, emits one `UnhandledException` diagnostic, and
aborts. Owning asynchronous or platform roots invoke it only after required
task, queue, state-publication, or shutdown bookkeeping is complete.

The public `ao::fatalFromException(exceptionPtr, context, location)` function
is the forwarding form for an owner helper that has already captured its
caller's `std::source_location`; ordinary call sites use the macro.

### Broad-catch audit marker

```cpp
AO_AUDITED_CATCH(reason)
```

`AO_AUDITED_CATCH` is a compile-time audit marker, not a fatal operation and not
permission to ignore an active failure. It is valid only as the first statement
of a broad catch whose owner has already established that continuation preserves
the primary state. The lint checker recognizes the local statement pattern rather
than function or file names.

The closed reason set is `ExceptionClassifier`, `FatalSinkFallback`,
`DiagnosticFallback`, `SafeCleanup`, `PreservePrimaryException`, and
`PlatformFallback`. Every production use and its ownership proof is listed in the
[exception-carrier reference](exception-carriers.md).

### Exception-carrier audit marker

```cpp
AO_EXCEPTION_CARRIER(reason)
```

`AO_EXCEPTION_CARRIER` is the first statement of a helper that owns one of the
narrow exception transports permitted by the
[exception-carrier reference](exception-carriers.md).
It is a compile-time audit marker and does not throw by itself.
The lint checker recognizes the macro expansion structurally, so renaming or
moving an inventoried helper does not require a checker allowlist change.

The closed reason set is `CancellationTransport`, `CommandBoundary`,
`PrivateErrorTransport`, and `ForeignCallbackAdapter`.
Adding a reason or a marked helper requires updating the exception-carrier
reference and its lint fixture in the same change.

### Raw-fatal backend marker

```cpp
AO_RAW_FATAL_BACKEND()
```

`AO_RAW_FATAL_BACKEND` is valid only as the first statement of the Core helper
that invokes a language or C-runtime process-termination primitive. It grants
no continuation or recovery behavior. The raw-fatal AST checker recognizes
the marker structurally; a direct helper call, nested marker, or later marker
does not qualify.

### Diagnostic payload

```cpp
struct FatalDiagnostic final
{
  FatalCategory category;
  std::string_view condition;
  std::string_view context;
  std::source_location location;
  bool contextTruncated;
};
```

`condition` is the stringized conditional expression and is empty for unconditional or exception-aware fatal entry.
`context` and `condition` remain valid only for the synchronous sink call; a sink must copy any data it retains.
`location` is the macro call site, or the deepest project exception location available to the exception-aware entry.
A contract helper that owns the condition instead of expanding the ordinary macro accepts a defaulted `std::source_location` at its caller-facing entry and uses the matching `_AT` form; the helper definition is not reported as the failing call site.

### Sink registration

```cpp
using FatalSink = bool (*)(FatalDiagnostic const&);

bool registerFatalSink(FatalSink sink) noexcept;
bool unregisterFatalSink(FatalSink sink) noexcept;
```

Exactly one non-null sink may be registered process-wide.
Registration succeeds only when the slot is empty.
Unregistration succeeds only when `sink` is the currently registered function.
The functions return `false` for a violated registration transition and do not replace or remove another owner.

The application registers after its logging backend is usable, keeps the callback and every borrowed logging object alive while registered, quiesces every possible caller, and unregisters before logger teardown.
The sink runs synchronously on the fatal caller's thread, may be called from any non-realtime thread, must not throw by contract, and returns whether it accepted the diagnostic.
The Core backend defensively contains an escaping sink exception and continues to abort.
Its result never changes the final abort.

## Validation rules

- Every fatal category remains enabled in every build configuration.
- Every non-recursive entry emits its own best-effort `AOBUS_FATAL` emergency record before competing for the application-sink slot.
- The emergency record contains the category token, condition when present, context when present, and source file plus line.
- A diagnostic that exceeds the emergency buffer ends with `diagnostic-truncated=true` and a newline; the marker replaces the discarded tail rather than being appended beyond the buffer.
- A recursive entry on the same thread emits a minimal `recursive-fatal` marker and aborts without re-entering formatting or the sink.
- Concurrent entries each attempt their complete emergency record; at most one enters the application sink, while every concurrent loser emits a minimal `concurrent-fatal` marker and aborts.
- Ordinary diagnostic formatting, emergency output, or sink failure never escapes and never prevents `std::abort()`.
- On Windows, the backend disables the Debug CRT abort dialog and Windows Error Reporting before `std::abort()`, so termination cannot block on UI.
- The ordinary sink path is bounded and non-blocking by application contract.
- The realtime form performs no heap allocation, dynamic formatting, application mutex acquisition, logger call, or explicit wait.
- Contract conditions and diagnostic arguments contain no operation required for program correctness.
- No public hook may replace abort with throw, return, or long jump.
- Raw `abort`, `terminate`, `quick_exit`, and `_Exit` references are forbidden
  outside the one structurally marked Core backend helper.
- Direct references to `ao::detail::abortFatal` and `ao::detail::abortRealtime` are forbidden outside the public Contract macro expansion; helper forwarding uses the public `_AT` forms.

## Compatibility and versioning

Macro names, fatal and audited-catch enumerators, diagnostic fields, and registration functions are source-level project API.
Changing them requires updating this reference, the outcome specification, every diagnostic adapter, and subprocess coverage.
Emergency record wording outside the `AOBUS_FATAL` marker and category token is diagnostic detail and has no compatibility guarantee.

## Examples

```cpp
AO_EXPECTS(buffer != nullptr, "Buffer must be supplied");
AO_ENSURES(view.isValid(), "Encoded view must validate");
AO_INVARIANT(activeGeneration != 0, "Active generation must be nonzero");

if (publicationResult < 0)
{
  AO_FATAL("Committed revision {} could not be published", revision);
}

AO_RT_INVARIANT(pushed, "RT signal ring capacity exceeded");
```

## Implementation authority

- The public declaration is owned by [`Contract.h`](../../../include/ao/Contract.h).
- The backend and emergency output are owned by [`Fatal.cpp`](../../../lib/utility/Fatal.cpp).
- The application logging adapter is owned by [`Log.cpp`](../../../app/runtime/Log.cpp).

## Test authority

- Core unit coverage lives under [`test/unit/core/`](../../../test/unit/core).
- Cross-platform subprocess coverage lives in the dedicated self-reentering
  `ao_fatal_probe`, `ao_audio_fatal_probe`, and `ao_library_probe` executables under
  [`test/fatal/`](../../../test/fatal). Runtime-only fatal scenarios self-reenter
  `ao_core_test`, whose normal parent run verifies the child diagnostic and termination.
- Runtime logging-adapter coverage lives in [`LogTest.cpp`](../../../test/unit/runtime/LogTest.cpp).
- [`ForbidRawFatalCheck.cpp`](../../../tool/lint/check/ForbidRawFatalCheck.cpp)
  and its [integration fixture](../../../test/integration/lint/fixture/aobus-readability-forbid-raw-fatal/BasicFixture.cpp)
  protect the structural raw-backend marker and distinguish true standard
  termination functions from unrelated project methods with the same leaf name.

## Related documents

- [Outcome channel specification](../../spec/failure/outcome-channel.md)
- [Failure and reporting architecture](../../architecture/failure-and-reporting.md)
- [Runtime execution architecture](../../architecture/runtime-execution.md)
- [Error value reference](error.md)
- [Exception carrier reference](exception-carriers.md)
- [Decision 0007](../../decision/0007-unify-fatal-diagnostics-and-abort.md)
