# Error Handling Model

## Status

Accepted.

## Purpose

This document defines the error-handling contract for Aobus across the public
core library, private application runtime, CLI, and GTK frontend. It exists so
new code does not have to choose ad hoc between throwing exceptions, wrapping
exceptions, returning `ao::Result<T>`, or returning `std::optional<T>`.

The decision is deliberately not a blanket no-exceptions policy. Aobus uses a
small set of mechanisms with distinct meanings:

- `ao::Result<T>` for recoverable failures the caller is expected to handle.
- `std::optional<T>` for legitimate absence, not failure.
- Exceptions for programmer errors, invariant violations, third-party callback
  mechanisms, and rare fatal startup defects.

## Decision Summary

Use error values as the normal cross-layer contract. Use exceptions only inside
well-defined boundaries or when the program contract has been violated.

```diagram
╭──────────────────────╮
│ include/ao + lib      │  public, frontend-neutral APIs
│ storage/audio/tag/db  │  recoverable failure -> ao::Result<T>
╰──────────┬───────────╯  absence -> std::optional<T>
           │
           ▼
╭──────────────────────╮
│ app/runtime           │  use-case APIs add context and return Result<Reply>
│ services/projections  │  batch item failures stay in result summaries
╰──────────┬───────────╯
           │
           ▼
╭──────────────────────╮
│ app/linux-gtk + CLI   │  consume Result, show notification/dialog/stderr
│ application shells    │  top-level catch is last-resort crash protection
╰──────────────────────╯
```

## Mechanism Selection

| Situation | Use | Notes |
| --- | --- | --- |
| Operation can legitimately fail and the caller should react | `ao::Result<T>` | IO, decode, config, import/export, device open, invalid user input. |
| Void operation can legitimately fail | `ao::Result<>` | Return `{}` for success. |
| Lookup/cache/query has no value and no error detail is needed | `std::optional<T>` | `get(id)`, `indexOf()`, optional metadata, cancelled dialog. |
| Missing value is itself an error for this API | `ao::Result<T>` with `Error::Code::NotFound` | Convert optional absence at the boundary that requires the value. |
| Caller violated an API contract | exception or assertion | Example: calling a write method on a read-only object. |
| Third-party API throws | catch at adapter/boundary and translate if recoverable | Add Aobus context when converting to `ao::Result<T>`. |
| Fatal built-in resource or packaging defect | exception is acceptable | Let top-level startup handling log/report it. Keep rare. |
| Async cancellation | subsystem cancellation mechanism | Root coroutine handlers should treat expected cancellation separately from errors. |

## `ao::Result<T>` Contract

Use `ao::Result<T>` for recoverable failures in public core and runtime APIs.
Recoverable means the caller can show an error, retry, skip an item, fall back,
or return an error to its own caller.

Recommended shapes:

```cpp
ao::Result<> openDevice(audio::DeviceId const& id);
ao::Result<audio::PcmBlock> readNextBlock();
ao::Result<ImportSummary> importLibrary(std::filesystem::path const& path);
ao::Result<UpdateTrackMetadataReply> updateMetadata(std::span<TrackId const> ids,
                                                    MetadataPatch const& patch);
```

Construction rules:

- Use `ao::makeError(code, message)` for concise errors.
- Return `{}` for successful `ao::Result<>`.
- The canonical spelling for successful `ao::Result<>` is `return {};`. The
  function return type carries the meaning. Do not return `std::nullopt`, a
  dummy success payload, or local one-off success helpers.
- Preserve the existing `Error::Code` when propagating an error unless the layer
  intentionally maps it to a more precise semantic code.
- Add context at layer boundaries. Until a shared helper exists, construct a new
  error with the old code and a prefixed message.

Example propagation with context:

```cpp
auto result = decoder.open(path);

if (!result)
{
  return ao::makeError(result.error().code,
                       std::format("Failed to open decoder for '{}': {}", path.string(), result.error().message));
}

return {};
```

Do not introduce `bool` plus `lastError()`, empty-string success, or
`std::optional<T>` failure reporting for new code.

## Construction and Initialization Failures

Constructors cannot return `ao::Result<T>`, so they should not perform
recoverable work whose failure the caller needs to handle as normal application
state.

Preferred patterns:

1. Use a static factory returning `ao::Result<T>` when callers should receive a
   fully initialized object or an error.
2. Use a cheap constructor plus an explicit `Result<> open()` or
   `Result<> initialize()` only when the object has a real lifecycle that must be
   represented separately, such as audio backends, decoder sessions, or objects
   that can be reopened.
3. Throw from a constructor only for programmer errors, invariant violations, or
   rare fatal startup defects. Do not make constructor exceptions the normal path
   for IO, device, database, or config failures.

Factory shape:

```cpp
class DeviceSession final
{
public:
  static ao::Result<DeviceSession> create(DeviceId const& id);

private:
  explicit DeviceSession(DeviceHandle handle);
};
```

Two-phase lifecycle shape:

```cpp
class DecoderSession final
{
public:
  explicit DecoderSession(DecoderOptions options);

  ao::Result<> open(std::filesystem::path const& path);
  ao::Result<PcmBlock> readNextBlock();
};
```

When using two-phase initialization, document or encode the valid states so
callers cannot accidentally use the object before a successful `open()` or
`initialize()`.

## `std::optional<T>` Contract

Use `std::optional<T>` only when absence is a valid non-error outcome and the
caller does not need an explanation.

Good examples:

```cpp
std::optional<TrackView> TrackStore::Reader::get(TrackId id) const;
std::optional<std::size_t> TrackListProjection::indexOf(TrackId trackId) const noexcept;
std::optional<TrackCustomViewDialog::Result> TrackCustomViewDialog::runDialog();
```

When a higher-level operation requires the value, convert absence to `Result<T>`
at that boundary:

```cpp
auto optView = reader.get(id);

if (!optView)
{
  return ao::makeError(ao::Error::Code::NotFound, "Track not found");
}

return *optView;
```

Do not use optional to hide parse, IO, database, or format failures. If the
caller needs to know what went wrong, return `ao::Result<T>`.

## Exception Contract

Exceptions are not the normal recoverable-error API across Aobus layers.

Allowed exception uses:

1. **Programmer error or invariant violation.** The caller broke the contract or
   the code reached an impossible internal state. Use
   `ao::throwException<ao::Exception>(code, message)`. Do not throw bare
   `std::logic_error` or other standard library exceptions from Aobus code;
   keeping all Aobus-thrown exceptions under one base type lets top-level
   handlers distinguish "our bug" from third-party library failures.
2. **Third-party callback or library mechanism.** Some libraries report failure
   by throwing, or need an exception-throwing callback to avoid aborting. Catch
   these at the adapter boundary when the failure is recoverable.
3. **Fatal startup defects.** Built-in resource corruption, missing compiled
   resources, or impossible packaging defects may throw to the top-level
   application guard. These should be rare and logged clearly.

Boundary translation example:

```cpp
ao::Result<> ConfigStore::ensureLoaded()
{
  try
  {
    _inputBuffer = yaml::readFile(_filePath);
    ryml::parse_in_place(yaml::toSubstr(_inputBuffer), &_root);
  }
  catch (std::exception const& e)
  {
    return ao::makeError(ao::Error::Code::IoError,
                         std::format("Failed to parse config file '{}': {}", _filePath.string(), e.what()));
  }

  return {};
}
```

Exception anti-patterns:

- Do not expose a public recoverable API that sometimes returns `Result<T>` and
  sometimes throws for the same category of failure.
- Do not catch exceptions in low-level implementation code only to turn them
  into strings with no actionable code or context.
- Do not rely on the GUI or CLI top-level catch for ordinary user-visible
  failures.

Top-level catch pattern:

```cpp
try
{
  return app.run(argc, argv);
}
catch (ao::Exception const& e)
{
  // Aobus invariant/logic/startup failure — our bug, logged with Error::Code.
  LOG_CRITICAL("Aobus fatal: [{}] {}", static_cast<int>(e.code), e.what());
  return EXIT_FAILURE;
}
catch (std::exception const& e)
{
  // Third-party library exception that escaped all adapter boundaries.
  LOG_CRITICAL("Unhandled third-party exception: {}", e.what());
  return EXIT_FAILURE;
}
```

The two catch blocks are deliberately separate: `ao::Exception` carries an
`Error::Code` for crash-dump triage; `std::exception` signals a missing adapter
translation that should eventually be fixed.

## Layer Responsibilities

### Public core library (`include/ao` + `lib`)

- Public fallible APIs should return `ao::Result<T>`.
- Public lookup APIs may return `std::optional<T>` when absence is normal.
- Implementation details may use exceptions for invariants or third-party
  callback control flow, but recoverable failures should be translated before
  crossing a public boundary.
- Error messages should be domain/infrastructure context, not GTK/CLI wording.

### Application runtime (`app/runtime`)

- Runtime services should expose recoverable use-case failures as
  `ao::Result<Reply>` or `ao::Result<>`.
- Runtime is the preferred place to add user-operation context such as
  "Failed to import library" or "Failed to update metadata".
- Batch operations should distinguish item-level failures from operation-level
  failures. If one bad file should not abort the import, store the item error in
  the summary instead of returning a failed `Result<Summary>`.
- Runtime should not leak GTK or CLI presentation concerns into `ao::Error`.

### GUI and CLI (`app/linux-gtk`, `app/cli`)

- Event handlers and commands should consume `ao::Result<T>` and show the error
  through notifications, dialogs, status text, or stderr.
- GUI code may catch `Glib::Error` or other frontend library exceptions at UI
  boundaries and translate them to user feedback or a runtime `Result<T>`.
- `main()` catch blocks are last-resort crash guards. They are not part of the
  normal error-handling path.

### Async runtime

- Root coroutine spawning owns unhandled-exception logging.
- Expected cancellation should be recognized and normally ignored or logged at a
  debug level, not reported as a user-visible failure.
- Async workflows that call fallible services should carry `ao::Result<T>` back
  to the UI thread and report it there.

Target shape for a UI-owned async workflow:

```cpp
rt::async::Task<void> ImportController::importAsync(std::filesystem::path path)
{
  co_await _async.resumeOnWorker();
  auto result = _importer.import(path); // ao::Result<ImportSummary>

  co_await _async.resumeOnControl();

  if (!result)
  {
    _notifications.post(rt::NotificationSeverity::Error,
                        std::format("Import failed: {}", result.error().message));
    co_return;
  }

  _notifications.post(rt::NotificationSeverity::Info, "Import complete");
}
```

Unexpected exceptions still belong to `spawnLogged()` or `spawnWithLifetime()`
root handlers. New async APIs should not use exceptions as the planned delivery
mechanism for recoverable failures. Legacy coroutine APIs that still throw for
recoverable work should catch at the UI workflow boundary and migrate toward
`Task<Result<T>>` or an internal `Result<T>` handoff.

## Error Message and Code Guidelines

- `Error::Code` is for programmatic dispatch, tests, and future UI mapping.
- `Error::message` carries context suitable for logs and, when appropriate,
  direct user display.
- Lower layers should report what failed technically. Higher layers may prefix
  the user operation context.
- Avoid duplicate logging and returning of the same error at every layer. Log at
  observation boundaries, or return the error for the caller to observe.
- Do not add new error codes speculatively. Add a code when callers need to
  branch or tests need to assert a distinct category.
- Avoid blindly prepending context at every layer. If a message already contains
  the user operation context, either return it unchanged or add only the missing
  context. A future shared helper may replace string prefixing with structured
  cause chaining; do not introduce one-off cause-chain types locally.

## Ignored Results and `[[nodiscard]]`

Aobus currently forbids ad hoc `[[nodiscard]]` annotations and relies on linting
and review for ignored-return diagnostics. Do not add `[[nodiscard]]` to
individual `Result<T>`-returning functions or wrapper classes.

If ignored `ao::Result<T>` values become a recurring bug source, prefer a
centralized enforcement mechanism, such as a custom clang-tidy rule for discarded
`ao::Result<T>`, over per-function attributes or a local wrapper introduced in
one subsystem.

## Migration Policy

Existing code may not fully follow this model. When touching an error path:

1. Keep the change scoped to the API being modified.
2. Use `ao::Result<T>` for new recoverable public/runtime APIs.
3. Keep existing `std::optional<T>` returns when they mean legitimate absence.
4. Convert third-party exceptions to `ao::Result<T>` at the nearest meaningful
   adapter boundary.
5. Leave invariant exceptions in place unless the state is actually a
   recoverable user/environment failure.
6. Avoid large mechanical rewrites whose only effect is replacing one mechanism
   with another without improving the API contract.

High-value cleanup targets:

- Functions that return `Result<T>` but can still throw for IO/config/library
  failures.
- APIs where `std::optional<T>` hides the reason for a user-visible failure.
- GUI/CLI handlers that rely on broad catch blocks instead of consuming a
  service result.

## Testing Expectations

Error-handling changes should include focused tests at the boundary whose
contract changed:

- `Result<T>` APIs: test success and representative error codes/messages.
- `std::optional<T>` APIs: test absence when absence is a meaningful outcome.
- Exception translation: test that recoverable thrown third-party failures are
  returned as `ao::Result<T>` with context.
- Batch workflows: test partial item failures separately from fatal operation
  failures.
- GUI/CLI changes: prefer service/controller tests where possible; use manual
  checks only when the behavior is purely visual or platform-interactive.

## Review Checklist

Before adding or changing a fallible API, answer these questions:

- Is the failure recoverable by the caller? If yes, return `ao::Result<T>`.
- Is there simply no value? If yes, return `std::optional<T>`.
- Is the caller violating an API contract? If yes, an invariant exception or
  assertion is acceptable.
- Does a third-party library throw? If yes, decide whether this function is the
  adapter boundary that should translate to `ao::Result<T>`.
- Will the GUI/CLI need to show this failure? If yes, preserve enough context in
  `ao::Error` and do not rely on top-level exception handling.
- Is this a batch operation where item errors should be accumulated instead of
  aborting the whole operation?
