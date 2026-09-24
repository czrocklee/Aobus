---
id: failure.exception-carriers
---
# Exception carrier reference

## Scope

This is the source-level whitelist for exception-shaped transport in production
Aobus code and repository tools. It records each project transport region, raw
throw helper, and broad catch that is allowed to continue. Exceptions not listed
here must be translated by an exact foreign adapter, transferred to an owning
boundary, rethrown after cleanup, or terminated through AO fatal handling.

Channel selection belongs to the [outcome channel specification](outcome-channel.md).
Fatal behavior belongs to the [fatal facility](fatal.md), and recoverable values
to the [error value reference](error.md).

## Project and private transports

| Carrier | Permitted region | Owning boundary and result |
|---|---|---|
| `ao::async::OperationCancelled` | Stop-aware async checkpoints and adapters that preserve cancellation. | A caller-owned future retains it. `spawnLogged`, `spawnCancellable`, and `spawnWithLifetime` consume it after their bookkeeping. |
| `ao::cli::CommandError` | One accepted CLI invocation. | `app/cli/Run.cpp` formats the command diagnostic and returns command-failure status. |
| `ao::query::detail::QueryException` | Query and format compilation helpers. | The source-private compiler boundary returns the carried `Error` as `Result`. |
| `ao::media::detail::MediaException` | MP4 demux construction. | The MP4 demux public boundary returns the carried `Error`. |
| `ao::audio::detail::DecoderException` | One decoder/session operation. | The nearest public decoder/session result boundary returns the carried `Error`. |
| `ao::library::detail::LibraryException` | Bounded record building and library-open admission helpers. | The enclosing builder, open, or root-write result owner returns the carried `Error`, after transaction cleanup where applicable. |
| `ao::lmdb::detail::TransactionFailure` | Failed native mutation. | The root `library::WriteTransaction` owner aborts and terminalizes first, then returns the carried `Error`. |
| Private RapidYAML callback carrier | One `parseInPlace()`, `parseInArena()`, or `resolve()` call in `RymlAdapter.cpp`. | The same adapter operation returns `FormatRejected`; partial parse state is discarded. |
| GTK layout-build carrier (`std::logic_error`) | `SharedWidgetHandoff` validation while `LayoutHost::prepare()` builds a candidate. | The handoff guard restores transferred widgets during unwind; `prepare()` returns `InitFailed`. The same audited adapter also contains standard exceptions from component construction. |

The project-specific error carriers derive directly from `std::exception`, own
an `Error`, and expose its message through `what()`. They are caught as their
exact leaf type; an unrelated exception is not reclassified as their error.
The GTK layout row is an adapter transport rather than a project Error carrier
and follows the explicitly stated adapter boundary instead.

No general `ao::Exception` hierarchy or public catch-all project vocabulary
exists.

## Raw throw helpers

Every production non-rethrowing `throw` is inside one of the helpers below, and
the helper's first statement is `AO_EXCEPTION_CARRIER(reason)`. Inline/template
overloads retain the same qualified helper name. The lint checker verifies the
statement shape rather than maintaining a site-name allowlist.

| Helper | Marker reason | Transport role |
|---|---|---|
| `ao::async::throwOperationCancelled` | `CancellationTransport` | Constructs cancellation. |
| `ao::cli::throwCommandError` | `CommandBoundary` | Constructs CLI command transport. |
| `ao::query::detail::throwQueryError` | `PrivateErrorTransport` | Constructs private query/format transport. |
| `ao::media::detail::throwMediaError` | `PrivateErrorTransport` | Constructs private media transport. |
| `ao::audio::detail::throwDecoderError` | `PrivateErrorTransport` | Constructs private decoder transport. |
| `ao::library::detail::throwLibraryError` | `PrivateErrorTransport` | Constructs private library transport. |
| `ao::lmdb::detail::throwTransactionFailure` | `PrivateErrorTransport` | Carries an existing mutation `Error`. |
| `ao::lmdb::throwOnMutationError` | `PrivateErrorTransport` | Converts a native LMDB mutation code. |
| `throwBasicParseFailure`, `throwDetailedParseFailure`, and `throwVisitFailure` in `RymlAdapter.cpp` | `ForeignCallbackAdapter` | Adapt RapidYAML callbacks to the private parser carrier. |
| `throwLayoutBuildError` in `LayoutHost.cpp` | `PrivateErrorTransport` | Unwinds a rejected GTK layout candidate through the handoff rollback guard. |

A bare `throw;` is used only to preserve the active exception after making owned
state safe. Tests may inject arbitrary exceptions to prove transport and fatal
behavior; those injections do not extend this production whitelist.

## Language and asynchronous transport

| Mechanism | Permitted role | Terminal owner |
|---|---|---|
| `std::exception_ptr` | Neutral transport for Boost.Asio completions, caller futures, and boundaries that must finish bookkeeping first. | `TaskFuture`, the CLI task pump, or a fire-and-forget terminal completion. |
| `std::bad_alloc` | Language allocation failure only; no current site translates it to `ResourceExhausted`. | Caller-owned exception channel or AO fatal root. |
| Other standard-library exception | A standard operation or an explicitly listed adapter region. | Exact recoverable adapter, audited executable boundary, or AO fatal root. |
| Third-party exception | Only inside the adapter for the throwing operation. | Exact adapter translation, required ABI catch, or AO fatal root. |
| Platform ABI exception | A framework callback that cannot permit C++ unwind. | Platform-declared recovery where one exists; otherwise exception-aware AO fatal entry. |

`std::exception_ptr` does not authorize the carried object. The underlying
exception still follows its carrier or adapter contract.

## Audited broad catches that continue

A broad catch may continue only at the sites below. Its first statement is
`AO_AUDITED_CATCH(reason)`. The marker has no runtime effect; it records the
local ownership proof for the lint checker.

| Site | Reason | Why continuation is safe |
|---|---|---|
| `async::isOperationCancelled(std::exception_ptr const&)` | `ExceptionClassifier` | It only classifies the carried object and owns no failed operation. |
| Fatal-sink invocation in `Fatal.cpp`; `rt::Log::trySubmitFatal()` | `FatalSinkFallback` | Abort is already inevitable; sink failure falls back to emergency diagnostics. |
| `LayoutHost::prepare()` | `DiagnosticFallback` | Candidate construction has failed; `SharedWidgetHandoff` restores transferred widgets before an `InitFailed` result is returned. |
| `ScopedTimer::~ScopedTimer()`; `PlaybackTransport::Impl::~Impl()` | `DiagnosticFallback` | Only post-operation timing or release logging is lost. |
| `ListOrderAuthoringSession::State::reconcileExceptionalSubmission()`; `TrackAuthoringSession::State::finishExceptionalSubmission()` | `PreservePrimaryException` | Submission state is settled first; observer failure must not replace the primary exception. |
| `App::showStartupFailure()` and the WinRT-detail fallback in `App::OnLaunched()` | `DiagnosticFallback` | Startup already failed; static Win32 text remains available. |
| `App::~App()` | `SafeCleanup` | Session/window state is released; only localization/logger cleanup remains. |
| `App::exitApplication()` | `PlatformFallback` | The process is already exiting; `PostQuitMessage` is the ABI fallback. |
| `checkpointWorkspaceBestEffort()` | `SafeCleanup` | The WinUI session is terminally tearing down; the optional checkpoint is diagnostic-only. |
| `reportOptionalWinRtFailure()` and `logWinUiCritical()` | `DiagnosticFallback` | These are diagnostic fallbacks; logger failure is reduced to `OutputDebugStringA`. |
| `clearKeymapAccelerators()` | `SafeCleanup` | Callers are abandoning handlers or already unwinding; handlers retain an owner check. A replacement path uses the throwing clear. |
| Catalog compiler `main()` | `DiagnosticFallback` | This is the tool's top-level command boundary; it prints a diagnostic and returns failure without preserving live application state. |

No active-operation settings save, ordinary observer, executor callback, thread
root, or platform ABI callback is a suppression exemption. Expected failures at
those boundaries use `Result` or an exact foreign-exception rule; every other
escape is rethrown/transferred or reaches AO fatal handling.

### Unresolved classification boundary

`LayoutHost::prepare()` currently catches `std::exception`, so its `InitFailed`
translation also catches `std::bad_alloc` and unrelated standard exceptions from
candidate construction. The rollback proof makes the active widget tree safe,
but it does not by itself justify classifying every such exception as a
recoverable layout failure. This conflicts with the existing general rule that
allocation and unrelated implementation faults are not laundered into domain
errors. The observed site is inventoried above, but it must not be copied as a
precedent until the layout boundary is narrowed or that broader classification
is explicitly approved.

## Validation rules

- Every marked helper and continuing broad catch appears in this reference.
- Marker reasons are the closed sets declared in `Contract.h`. Adding a reason,
  marked helper, or audited catch updates this reference, focused boundary
  coverage, and the lint fixture in the same change.
- A recoverable public API never requires callers to catch a project exception.
- Fire-and-forget roots consume only cancellation, retire task/scope/queue
  bookkeeping, and pass every other escape to `AO_FATAL_EXCEPTION()`.
- Caller-owned tasks preserve the original exception and are not also diagnosed
  as unobserved.
- Cleanup catches suppress only explicitly best-effort work after primary state
  is safe; they never suppress the active operation fault.

## Authority

Carrier declarations live with their owning subsystem; cancellation is in
`include/ao/async/`, CLI transport in `app/cli/`, and private Error carriers in
the corresponding `detail/` directories. `RymlAdapter.cpp` and `LayoutHost.cpp` own the adapter-only rows.

`ForbidRawThrowCheck.cpp` and its integration fixture enforce the marker shapes.
Subsystem tests protect public translation. `AsyncRuntimeTest.cpp`,
`LifetimeScopeTest.cpp`, and `CliRuntimeTest.cpp` protect terminal ownership;
`LayoutHostTest.cpp` protects atomic GTK candidate handoff; fatal subprocess
scenarios protect exception-aware process termination.

## Related documents

- [Outcome channel specification](outcome-channel.md)
- [Failure and reporting architecture](README.md)
- [Runtime execution architecture](../execution/README.md)
- [Fatal facility reference](fatal.md)
- [Error value reference](error.md)
- [Decision 0007](../../decision/0007-unify-fatal-diagnostics-and-abort.md)
