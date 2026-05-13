# Design, Threading, and Error-Handling Snippets

Covers `CONTRIBUTING.md` rules 3.3, 4.1-4.4, and 5.1-5.3.

## `std::expected` via `ao::Result` (3.3, 5.2)

```cpp
ao::Result<> open(DeviceId const& id)
{
  if (id.empty())
  {
    return ao::makeError(ao::Error::Code::DeviceNotFound, "Missing device id");
  }

  if (auto const result = connect(id); !result)
  {
    return std::unexpected(result.error());
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
- Use `ao::makeError(code, message)` for concise error results, or `std::unexpected(ao::Error{...})` when explicit construction is clearer.
- Do not use `bool` + `lastError()`, empty strings, `std::optional`, or `std::error_code` for new recoverable error reporting.

## Three-layer error policy (5.1, 5.3)

`ao::Result<T>`: recoverable and caller-handled:

```cpp
ao::Result<Device> findRequiredDevice(DeviceId const& id);
```

`ao::Exception` through `AO_THROW` / `AO_THROW_FORMAT`: invariant violations, data corruption, impossible states, unrecoverable failures:

```cpp
if (header.magic != kExpectedMagic)
{
  AO_THROW_FORMAT(ao::Exception, "Invalid track header magic: {}", header.magic);
}
```

`std::optional<T>`: legitimate absence, with `opt` naming:

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

Catch exceptions at boundary points only:

```cpp
void ImportWorker::run() noexcept
{
  try
  {
    importLibrary();
  }
  catch (ao::Exception const& ex)
  {
    IMPORT_LOG_ERROR("Import failed: {}", ex.what());
  }
}
```

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
    auto lock = std::lock_guard{_bufferMutex};
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
- Use `std::mutex` + `std::lock_guard` for shared state; use `std::unique_lock` only when its extra behavior is needed.
- Use `std::atomic` for simple cross-thread flags/counters; never use `volatile` for synchronization.
