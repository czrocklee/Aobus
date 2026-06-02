# Design, Threading, and Error-Handling Snippets

Covers `CONTRIBUTING.md` rules 3.3, 4.1-4.4, and 5.1-5.3.

## Error model source of truth (3.3, 5.1-5.3)

Read `../../../../doc/design/error-handling.md` before adding or changing a fallible API.

Mechanism meanings:

- `ao::Result<T>`: recoverable failure the caller should handle.
- `std::optional<T>`: legitimate absence with no failure detail needed.
- Exception: programmer error, invariant violation, third-party callback mechanism, or rare fatal startup defect.

Do not mix mechanisms for the same failure category on the same API.

## `std::expected` via `ao::Result` (3.3, 5.1, 5.2)

```cpp
ao::Result<> open(DeviceId const& id)
{
  if (id.empty())
  {
    return ao::makeError(ao::Error::Code::DeviceNotFound, "Missing device id");
  }

  if (auto const result = connect(id); !result)
  {
    return ao::makeError(result.error().code, std::format("Failed to connect device: {}", result.error().message));
  }

  return {};
}

ao::Result<PcmBlock> readNextBlock()
{
  if (_decoderReachedEof)
  {
    return ao::makeError(ao::Error::Code::NotFound, "No more PCM blocks");
  }

  return PcmBlock{.bytes = readBytes(), .endOfStream = false};
}
```

- Use `ao::Result<T>` for recoverable failures and `ao::Result<>` for void success/failure.
- Return `{}` for successful `ao::Result<>`.
- Treat `return {};` as the canonical successful `ao::Result<>` spelling; do not invent local success helpers.
- Use `ao::makeError(code, message)` for concise error results, or `std::unexpected(ao::Error{...})` when explicit construction is clearer.
- Preserve `Error::Code` when adding context unless the current layer intentionally maps the error category.
- Do not use `bool` + `lastError()`, empty strings, `std::optional`, or `std::error_code` for new recoverable error reporting.
- Do not add `[[nodiscard]]` to functions or regular classes. RAII classes that manage scope or resource handles MUST include `[[nodiscard]]`. This is enforced via lint for classes with the following suffixes: `Guard`, `Subscription`, `Scope`, `Session`, `Lock`, `Transaction`, `Timer`, `Writer`, `Handle`, `Token`, `TempDir`, `TempFile`.

## Optional means absence (3.2.1, 5.1.3, 5.3)

```cpp
std::optional<Device> optDefaultDevice(std::span<Device const> devices)
{
  auto const iter = std::ranges::find(devices, true, &Device::isDefault);

  if (iter == devices.end())
  {
    return std::nullopt;
  }

  return *iter;
}
```

- Use the `opt` prefix for optional variables, members, and parameters.
- Use `if (optValue)` / `if (!optValue)` checks; do not use `.has_value()`.
- Convert absence to `ao::Result<T>` only at the boundary that requires a value.

```cpp
ao::Result<Device> findRequiredDevice(DeviceId const& id)
{
  auto optDevice = findDevice(id);

  if (!optDevice)
  {
    return ao::makeError(ao::Error::Code::DeviceNotFound, "Device not found");
  }

  return *optDevice;
}
```

## Exception boundaries (5.1.2, 5.3.4)

Use `ao::Exception` for invariant or contract violations:

```cpp
void ConfigStore::save(std::string_view group, State const& state)
{
  if (_mode == OpenMode::ReadOnly)
  {
    ao::throwException<ao::Exception>(ao::Error::Code::InternalError,
                                      "save() called on ReadOnly ConfigStore");
  }

  // Save implementation...
}
```

Catch third-party exceptions at adapter boundaries and convert to `ao::Result<T>` when the failure is recoverable:

```cpp
ao::Result<> loadConfig(ConfigStore& store, Config& config)
{
  try
  {
    return store.load("config", config);
  }
  catch (std::exception const& ex)
  {
    return ao::makeError(ao::Error::Code::IoError, std::format("Failed to load config: {}", ex.what()));
  }
}
```

Root async tasks and application entry points may catch broad exceptions, but only as last-resort logging/crash protection. GUI and CLI handlers should consume service `ao::Result<T>` values instead of relying on top-level catch blocks.

## Construction and async boundaries

Constructors cannot return `ao::Result<T>`. Prefer a static factory for recoverable construction failures:

```cpp
class DeviceSession final
{
public:
  static ao::Result<DeviceSession> create(DeviceId const& id);

private:
  explicit DeviceSession(DeviceHandle handle);
};
```

Use `Result<> open()` or `Result<> initialize()` only when the type has a real two-phase lifecycle, such as decoder sessions or audio backends. Do not throw from constructors for ordinary IO/device/config failures.

For async workflows, carry recoverable failures back to the control thread as values:

```cpp
rt::async::Task<void> ImportController::importAsync(std::filesystem::path path)
{
  co_await _async.resumeOnWorker();
  auto result = _importer.import(path);

  co_await _async.resumeOnControl();

  if (!result)
  {
    _notifications.post(rt::NotificationSeverity::Error, result.error().message);
    co_return;
  }

  _notifications.post(rt::NotificationSeverity::Info, "Import complete");
}
```

Root coroutine exception handlers are for unexpected exceptions and expected cancellation, not for normal recoverable failure delivery in new APIs.

## Getters and accessors (4.1)

Keep trivial one-line getters and setters inline in headers:

```cpp
class Metadata final
{
public:
  std::string const& title() const noexcept { return _title; }
  void setTitle(std::string title) { _title = std::move(title); }

private:
  std::string _title;
};
```

## Class design and exposure (4.2)

Prefer `final` for concrete classes and POD-like structs:

```cpp
struct TrackHeader final
{
  std::uint64_t trackId = 0;
  std::uint32_t durationMs = 0;
};

class TrackView final
{
public:
  explicit TrackView(TrackHeader const& header);
};
```

Keep implementation-only types in `.cpp` anonymous namespaces:

```cpp
namespace
{
  struct SortKey final
  {
    std::string normalizedTitle{};
    std::uint64_t trackId = 0;
  };
} // namespace
```

Use Pimpl for complex implementation details:

```cpp
// Header
class PipeWireBackend final : public IBackend
{
public:
  PipeWireBackend();
  ~PipeWireBackend() override;

private:
  struct Impl;

  std::unique_ptr<Impl> _impl;
};

// .cpp
struct PipeWireBackend::Impl final
{
  PwThreadLoopPtr threadLoop;
  PwStreamPtr stream;
};
```

## Const correctness (4.3)

```cpp
void addTrack(Track const& track)
{
  auto const key = buildSortKey(track);
  _tracks.emplace(key, track);
}

std::size_t TrackStore::size() const noexcept
{
  return _tracks.size();
}

void configure(Logger& logger, Config const& config); // mandatory services by reference
```

## Threading (4.4)

```cpp
class StreamingSource final
{
public:
  void startDecodeThread()
  {
    _decodeThread = std::jthread{
      [this](std::stop_token const& token)
      {
        ao::setCurrentThreadName("StreamingSource-Decode");
        decodeLoop(token);
      }};
  }

  std::size_t bufferedBytes() const noexcept
  {
    auto lock = std::scoped_lock{_bufferMutex};
    return _buffer.size();
  }

private:
  void decodeLoop(std::stop_token const& token);

  mutable std::mutex _bufferMutex;
  std::vector<std::byte> _buffer;
  std::jthread _decodeThread;
  std::atomic<bool> _failed = false;
};
```

- Name background threads with `ao::setCurrentThreadName()`.
- Use `std::jthread`/`std::stop_token`; do not roll manual stop flags.
- Use `std::mutex` + `std::scoped_lock` for shared state; use `std::unique_lock` only when its extra behavior is needed.
- Use `std::atomic` for simple cross-thread flags/counters; never use `volatile` for synchronization.
