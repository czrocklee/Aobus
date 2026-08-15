---
id: failure.exception-carriers
type: reference
status: current
domain: system
summary: Enumerates the exact exception carriers, throw regions, catch owners, and terminal transport permitted in Aobus.
---
# Exception carrier reference

## Scope and version

This reference is the exhaustive whitelist for exception-shaped transport in production Aobus code.
It names each project carrier, its permitted throw region, its owning catch boundary, and the channel produced by that boundary.
It also records the limited roles of `std::exception_ptr`, `std::bad_alloc`, standard-library exceptions, third-party exceptions, and platform ABI exceptions.

Outcome selection belongs to the [outcome channel specification](../../spec/failure/outcome-channel.md).
Fatal diagnostics belong to the [fatal facility reference](fatal.md), and recoverable values belong to the [error value reference](error.md).

## Code boundary

The [system architecture](../../architecture/system-overview.md) places cancellation and task transport in Core async code, private subsystem carriers inside their owning implementations, and `CommandError` inside the CLI frontend.
No general Aobus exception base or public catch-all exception vocabulary exists.
A project carrier may cross only the narrow implementation region named below and must not escape the public subsystem boundary that owns its translation.

## Project carriers

| Carrier | Visibility and throw region | Exact catch owner | Produced channel |
|---|---|---|---|
| `ao::async::OperationCancelled` | Public async control-flow type; thrown only by stop-aware async checkpoints and adapters that preserve cancellation. | Caller-owned future, or the terminal completion of `Runtime::spawnLogged`, `spawnCancellable`, or `spawnWithLifetime`. | Original exception for caller-owned work; otherwise silent completion after mandatory bookkeeping. |
| `ao::cli::CommandError` | CLI-local type; thrown only while implementing one accepted CLI invocation. | Top-level command runner in `app/cli/Run.cpp`. | Command diagnostic on stderr and the CLI command-failure exit status. |
| `ao::query::detail::QueryException` | Query implementation detail; thrown by query and format compilation helpers. | The translation in the source-private `QueryCompiler::compile()` or `FormatCompiler::compile()` immediately below `compileQuery()` or `compileFormat()`. | Carried `Error` as `Result`. |
| `ao::media::detail::MediaException` | Encoded-media implementation detail; thrown while constructing an MP4 demux result. | The MP4 demux public result boundary. | Carried `Error` as `Result`. |
| `ao::audio::detail::DecoderException` | Decoder implementation detail; thrown within one decoder/session operation. | The nearest decoder/session public result boundary that owns that operation. | Carried `Error` as `Result`. |
| `ao::library::detail::LibraryException` | Library implementation detail; thrown by bounded record-building or open-admission helpers. | The enclosing builder, library-open, or root write result boundary named by the throwing region. | Carried `Error` as `Result`, after transaction cleanup when applicable. |
| `ao::lmdb::detail::TransactionFailure` | LMDB implementation detail; thrown only to unwind a failed native mutation. | The owning root `library::WriteTransaction` operation. | Transaction abort/terminalization followed by the carried `Error` as `Result`. |
| Private RapidYAML callback carrier | Defined only in `lib/utility/RymlAdapter.cpp`; thrown only from callbacks installed for one `parseInPlace()`, `parseInArena()`, or `resolve()` call. | That same adapter operation. | `FormatRejected` as `Result<>`; partial parse state is unusable. |

Every error-carrying leaf type derives directly from `std::exception`, owns its `Error`, and returns the owned message from `what()`.
Inheritance between project carriers is forbidden because it permits a broader catch owner than the table declares.

## Raw throw regions

Production non-rethrowing `throw` expressions exist only in these exact helpers.
Every helper begins with `AO_EXCEPTION_CARRIER(reason)`.
The lint checker validates that first-statement macro pattern from the AST, so
this inventory remains the human authority without being copied into a
function-name or file allowlist in the checker.
Overloads and function templates retain the same qualified helper name.

| Exact helper | Marker reason | Carrier or adapter role |
|---|---|---|
| `ao::async::throwOperationCancelled` | `CancellationTransport` | Constructs the cancellation carrier. |
| `ao::cli::throwCommandError` | `CommandBoundary` | Constructs the CLI command carrier. |
| `ao::query::detail::throwQueryError` | `PrivateErrorTransport` | Constructs the private query carrier. |
| `ao::media::detail::throwMediaError` | `PrivateErrorTransport` | Constructs the private media carrier. |
| `ao::audio::detail::throwDecoderError` | `PrivateErrorTransport` | Constructs the private decoder carrier. |
| `ao::library::detail::throwLibraryError` | `PrivateErrorTransport` | Constructs the private library carrier. |
| `ao::lmdb::detail::throwTransactionFailure` | `PrivateErrorTransport` | Constructs a native mutation carrier from an existing `Error`. |
| `ao::lmdb::throwOnMutationError` | `PrivateErrorTransport` | Constructs a native mutation carrier from an LMDB code. |
| File-local `throwBasicParseFailure`, `throwDetailedParseFailure`, and `throwVisitFailure` in `lib/utility/RymlAdapter.cpp` | `ForeignCallbackAdapter` | Adapt RapidYAML error callbacks to the private parser carrier. |
| File-local `ao::gtk::platform::throwGioError` in `MprisBridge.cpp` | `ForeignCallbackAdapter` | Raises the exact `Gio::Error` required by the MPRIS property callback adapter. |

Cleanup uses a bare `throw;` only to preserve the active exception after making the owned state safe.
Tests may construct arbitrary exceptions to prove transport and fatal behavior; that test-only injection does not extend the production whitelist.

## Language and asynchronous transport

| Mechanism | Permitted role | Terminal owner |
|---|---|---|
| `std::exception_ptr` | Neutral transport used by Boost.Asio completions, caller-owned futures, and an owning boundary that must finish bookkeeping before rethrow or fatal handling. | Caller-owned `TaskFuture`, CLI task pump, or a fire-and-forget terminal completion. |
| `std::bad_alloc` | Language-runtime allocation failure transport only; no current site translates it to `ResourceExhausted`. | The nearest caller-owned exception owner or AO fatal root after mandatory cleanup. |
| Other standard-library exception | May escape a standard operation only until the narrow adapter that owns that operation, or until an owning fatal root when no recoverable adapter contract exists. | Adapter-specific `Result`/fallback policy, or `AO_FATAL_EXCEPTION()` for an unexpected escape. |
| Third-party exception | May exist only inside the adapter for the third-party operation that can throw. | Exact adapter translation, a required platform ABI catch, or an owning fatal root. |
| Platform ABI exception | May be caught at the framework/ABI callback boundary that cannot permit C++ unwinding across it. | Platform-declared recoverable result when one exists; otherwise AO exception-aware fatal entry. |

`std::exception_ptr` does not make an otherwise forbidden exception permissible.
The carried object still follows the project-carrier or foreign-adapter rules above when observed.

## Validation rules

- Production code defines no `ao::Exception`, `ExceptionFormat`, or generic project throw helper.
- Every non-rethrowing production `throw` is inside an inventoried helper whose
  first statement is `AO_EXCEPTION_CARRIER(reason)`; directly invoking the
  marker implementation or placing the macro later or inside control flow is invalid.
- A recoverable public API does not require callers to catch a project exception.
- A private carrier is caught by exact type; `catch (std::exception const&)` does not translate it into a domain `Error`.
- An unrelated exception is never laundered into the error code carried by a private leaf exception.
- Fire-and-forget roots consume only `OperationCancelled`.
  Every other escaping exception reaches `AO_FATAL_EXCEPTION()` after
  mandatory task, scope, queue, and shutdown bookkeeping.
- Caller-owned tasks preserve the original exception for their caller and never also report it as unobserved.
- No current `std::bad_alloc` catch converts allocation failure into a recoverable Aobus error.
- Destructor and cleanup catches may suppress only explicitly best-effort cleanup whose owner has already made the primary state safe; they never suppress an active operation fault.

## Audited broad-catch exemptions

The following sites are the complete production inventory permitted to continue
after a broad catch. Each catch begins with `AO_AUDITED_CATCH(reason)`. The macro
has no runtime policy effect; it makes the local ownership fact explicit and lets
`aobus-readability-forbid-raw-throw` verify the catch structurally without a
function-name or file allowlist. A broad catch outside this table must rethrow,
transfer an `exception_ptr` to an owning boundary, or enter AO fatal handling.

| Site | Marker reason | Why continuation is valid |
|---|---|---|
| `async::isOperationCancelled(std::exception_ptr const&)` in `OperationCancelled.cpp` | `ExceptionClassifier` | The function is an exception classifier. It returns whether the carried object is cancellation and neither executes nor owns the failed operation. |
| Fatal-sink invocation in `Fatal.cpp` | `FatalSinkFallback` | The process is already terminating. A throwing application sink is replaced by an emergency `sink=exception` marker before abort. |
| `rt::Log::submitFatal()` | `FatalSinkFallback` | This is the application adapter called by the Core fatal backend. Logger rejection is returned as `false`, allowing the already-active fatal path to use its emergency sink. |
| `App::showStartupFailure()` and the WinRT-detail fallback in `App::OnLaunched()` | `DiagnosticFallback` | Startup has already failed and no live session is being preserved. Static Win32 text remains available when localized presentation itself fails. |
| `App::~App()` | `SafeCleanup` | All owned window/session state is already released. Logger shutdown is diagnostic-only. |
| `App::exitApplication()` | `PlatformFallback` | The process is already exiting, and `PostQuitMessage` is the no-throw ABI fallback for a failed XAML exit request. |
| `checkpointWorkspaceBestEffort()` in `LibrarySession.cpp` | `SafeCleanup` | The session is already in teardown and cannot resume. The checkpoint is optional; its diagnostic uses the no-throw WinUI debugger fallback. |
| `reportOptionalWinRtFailure()` and `logWinUiCritical()` in `WinUiErrorBoundary.cpp` | `DiagnosticFallback` | These functions are the diagnostic fallback itself. A logger failure is reduced to `OutputDebugStringA`, which does not re-enter the logger. |
| `TrackAuthoringSession::Impl::finishExceptionalSubmission()` | `PreservePrimaryException` | A primary submission exception is already active. The secondary invalidation changes state before notifying; a notification failure must not replace the primary exception. |
| `ScopedTimer::~ScopedTimer()` | `DiagnosticFallback` | Timing output is diagnostic-only after the measured scope has completed; logger failure cannot affect the completed operation. |
| `PlaybackTransport::Impl::~Impl()` | `DiagnosticFallback` | Playback shutdown has completed before release logging. Failure to format or submit that informational record cannot alter resource release. |
| `clearKeymapAccelerators()` in `KeymapAccelerators.cpp` | `SafeCleanup` | This entry point exists to abandon accelerators, not to replace them: its callers are an owner releasing the invoker they hold, and an install unwinding with an exception already in flight. No live operation depends on the result, and the handlers check their owner before running. An install that needs the clear to have succeeded uses the throwing form instead. |

No active-operation settings save, callback, observer, executor, thread root, or
platform ABI callback is a suppression exemption. Expected failures at those
boundaries use `Result` or an exact foreign exception policy; every other escape
enters AO fatal handling with boundary context.

## Compatibility and versioning

This is a source-level whitelist rather than a serialized contract.
Adding a project carrier, widening a throw region, adding an exception-carrier or
audited-catch marker reason, adding an audited catch, or introducing a recoverable
`std::bad_alloc` translation requires updating this reference, the owning
specification, tests, and mechanical guardrails in the same change.

## Implementation authority

- [`OperationCancelled.h`](../../../include/ao/async/OperationCancelled.h), [`Runtime.cpp`](../../../lib/async/Runtime.cpp), and [`LifetimeScope.cpp`](../../../lib/async/LifetimeScope.cpp) own cancellation and terminal async observation.
- [`CommandError.h`](../../../app/cli/CommandError.h) and [`Run.cpp`](../../../app/cli/Run.cpp) own CLI exception transport.
- Private subsystem carrier declarations live under their owning `detail/` directories.
- [`RymlAdapter.cpp`](../../../lib/utility/RymlAdapter.cpp) owns the private RapidYAML callback carrier and its immediate translation.
- [`Contract.h`](../../../include/ao/Contract.h) and [`Fatal.cpp`](../../../lib/utility/Fatal.cpp) own exception-aware fatal termination.

## Test authority

- Subsystem compiler, parser, decoder, builder, and transaction tests protect each private carrier's public translation.
- [`AsyncRuntimeTest.cpp`](../../../test/unit/runtime/AsyncRuntimeTest.cpp), [`LifetimeScopeTest.cpp`](../../../test/unit/runtime/LifetimeScopeTest.cpp), and [`CliRuntimeTest.cpp`](../../../test/unit/cli/CliRuntimeTest.cpp) protect caller ownership, cancellation, and mandatory bookkeeping.
- Runtime fatal subprocess scenarios under [`test/fatal/`](../../../test/fatal) protect exception-aware termination during normal operation and shutdown.
- [`ForbidRawThrowCheck.cpp`](../../../tool/lint/check/ForbidRawThrowCheck.cpp) and its [integration fixture](../../../test/integration/lint/fixture/aobus-readability-forbid-raw-throw/BasicFixture.cpp) protect the first-statement exception-carrier and audited-catch marker patterns without site-name allowlists.
- The raw-fatal AST checker protects the exact standard termination symbols
  and the structural `AO_RAW_FATAL_BACKEND()` exception; a build-owned lexical
  guard provides an earlier common-spelling failure outside the Core backend.

## Related documents

- [Outcome channel specification](../../spec/failure/outcome-channel.md)
- [Failure and reporting architecture](../../architecture/failure-and-reporting.md)
- [Runtime execution architecture](../../architecture/runtime-execution.md)
- [Fatal facility reference](fatal.md)
- [Error value reference](error.md)
- [Decision 0007](../../decision/0007-unify-fatal-diagnostics-and-abort.md)
