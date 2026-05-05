# Unified Error Handling Strategy

## Motivation

The codebase has accumulated five distinct error handling idioms with no clear policy.
This document defines a unified strategy based on **`std::expected`** for recoverable
fallible operations and **`ao::Exception`** for invariant violations, and lays out a
phased migration plan.

---

## Current State: 5 Distinct Patterns

### Pattern 1 — `bool` return + `lastError()` side-channel

Used across the entire audio pipeline:

| Interface | Return | Error Channel |
|---|---|---|
| `IAudioBackend::open()` | `bool` | `lastError() → string_view` |
| `IAudioDecoderSession::open()` | `bool` | `lastError() → string_view` |
| `IAudioDecoderSession::seek()` | `bool` | `lastError() → string_view` |
| `IPcmSource::seek()` | `bool` | `lastError() → string` |
| `StreamingPcmSource::initialize()` | `bool` | `lastError()` |
| `MemoryPcmSource::initialize()` | `bool` | `lastError()` |
| `PlaybackEngine::openTrack()` | `bool` | `_snapshot.statusText` |

**Problems:**
- Error info is detached from the return value — callers must remember to query a second method.
- Thread safety is fragile (`_lastError` mutated from callbacks, read from callers).
- Error strings are not structured — no error codes, no programmatic handling possible.
- `lastError()` lifetime/ownership differs: some return `string_view` (dangling risk), some return `string` (allocation).

### Pattern 2 — `std::optional` return + `lastError()` side-channel

| Interface | Return | Error Channel |
|---|---|---|
| `IAudioDecoderSession::readNextBlock()` | `optional<PcmBlock>` | `lastError()` |

**Problems:** Same as Pattern 1 but worse — `nullopt` is ambiguous (could it also
mean "no more data"?). The caller in `StreamingPcmSource::decodeNextBlock()` must
check `nullopt` vs `block->endOfStream` to distinguish error from EOF.

### Pattern 3 — Empty `std::string` as error indicator

| Call site | Semantics |
|---|---|
| `MappedFile::map()` | Returns `""` on success, error message on failure |

**Problems:** Inverted semantics (empty = success). Callers check `!mapError.empty()`
which is unintuitive. No type safety.

### Pattern 4 — `ao::Exception` via `AO_THROW` / `AO_THROW_FORMAT`

Heavily used in `lib/`:

| Subsystem | Count | Examples |
|---|---|---|
| LMDB wrappers | ~20 | `throwOnError()` in `ThrowError.h` |
| Expression engine | ~15 | `Parser.cpp`, `ExecutionPlan.cpp` |
| Core library stores | ~10 | `MusicLibrary.cpp`, `DictionaryStore.cpp`, `ListView.cpp` |
| Tag parsing | ~8 | FLAC `MetadataBlock.cpp`, ID3v2 `Frame.h` |
| Library import/export | ~12 | `Importer.cpp`, `Exporter.cpp` |

**Usage is appropriate here** — these are invariant violations (corrupted data,
programmer errors, unrecoverable I/O). The `app` layer catches them at boundary points:
- `MainWindow::openMusicLibrary` catches `std::exception`
- `ImportWorker::run` catches per-file
- `MainWindow` export/import flows catch and dispatch to UI

### Pattern 5 — `std::optional` for legitimate "not found" semantics

| Call site | Semantics |
|---|---|
| `TrackStore::Reader::get()` | Track may not exist |
| `ListStore::Reader::get()` | List may not exist |
| `lmdb::Reader::get()` | Key may not exist |
| `PipeWireMonitor::findSinkIdByName()` | Sink may not exist |

**This usage is correct** — `optional` is the right tool for "absence is not an error".

---

## Target Policy

### Layer 1: `ao::Exception` (unchanged) — Invariant violations

**When:** Data corruption, programmer error, impossible states, unrecoverable
system failures.

Keep `AO_THROW` / `AO_THROW_FORMAT` exactly as they are for:
- LMDB operation failures (disk corruption, resource exhaustion)
- Expression parser/evaluator internal errors
- Tag parsing of malformed files
- Library import/export format violations
- Data store invariant violations

These are **not** expected failures in normal operation. The `app` layer already
catches them at boundary points — that architecture is sound.

### Layer 2: `std::expected<T, ao::Error>` (new) — Recoverable fallible operations

**When:** The operation can legitimately fail, the caller is expected to handle
the failure, and the error carries useful context.

Replace Patterns 1, 2, and 3 with `std::expected` in these interfaces:

| Current | Proposed |
|---|---|
| `bool open(...)` + `lastError()` | `std::expected<void, Error> open(...)` |
| `bool seek(...)` + `lastError()` | `std::expected<void, Error> seek(...)` |
| `bool initialize()` + `lastError()` | `std::expected<void, Error> initialize()` |
| `optional<PcmBlock> readNextBlock()` + `lastError()` | `std::expected<PcmBlock, Error> readNextBlock()` |
| `std::string map(path)` | `std::expected<void, Error> map(path)` |

### Layer 3: `std::optional<T>` (unchanged) — Legitimate absence

Keep `std::optional` for lookups where "not found" is a normal outcome, not an error.

### Why NOT `std::error_code` / `std::error_category`

`std::error_code`/`std::error_category` is designed for **cross-library error interop**
(e.g., Boost.Asio errors vs POSIX errno vs `std::filesystem` errors flowing through
a single type-erased `std::error_code`). Aobus doesn't need this because:

1. **No cross-library error boundary.** All error producers and consumers are in
   the same codebase.
2. **`error_code` can't carry a message.** It's just `int` + `category*`. You'd
   still need a wrapper struct with a message string.
3. **The C APIs we interop with don't use it.** LMDB, ALSA, PipeWire, libFLAC
   all have their own error representations.
4. **Significant boilerplate for zero benefit.** Each domain needs a category
   singleton, `make_error_code()` ADL overload, and `is_error_code_enum`
   specialization.

---

## Error Type Design

A single lightweight value type in `include/rs/Error.h`:

```cpp
#pragma once

#include <cstdint>
#include <string>

namespace ao
{
  struct Error final
  {
    enum class Code : std::uint8_t
    {
      Generic,
      DeviceNotFound,
      FormatRejected,
      InitFailed,
      IoError,
      DecodeFailed,
      SeekFailed,
      NotSupported,
    };

    Code code = Code::Generic;
    std::string message;
  };
} // namespace ao
```

This gives programmatic dispatch on `code` plus rich contextual `message`, all
carried inline in `std::expected<T, ao::Error>` — no side-channel needed.

If a subsystem later needs finer-grained codes, it can define its own error type
while staying in the `std::expected` framework.

---

## Execution Plan

### Phase 1 — Audio Pipeline (`app/core/`)

The highest-impact area. Most `bool + lastError()` and `optional + lastError()`
patterns live here.

#### 1.1 — New error type

- **[NEW]** `include/rs/Error.h` — the `ao::Error` struct above.

#### 1.2 — Decoder interface and implementations

- **[MODIFY]** `app/core/decoder/IAudioDecoderSession.h`
  - `bool open(...)` → `std::expected<void, ao::Error> open(...)`
  - `bool seek(...)` → `std::expected<void, ao::Error> seek(...)`
  - `optional<PcmBlock> readNextBlock()` → `std::expected<PcmBlock, ao::Error> readNextBlock()`
  - Remove `lastError()`

- **[MODIFY]** `app/core/decoder/FlacDecoderSession.h` / `.cpp`
- **[MODIFY]** `app/core/decoder/AlacDecoderSession.h` / `.cpp`
  - Update implementations to return `expected` directly instead of setting `_impl->error`.
  - Remove `_error` / `_lastError` members.

#### 1.3 — Backend interface and implementations

- **[MODIFY]** `app/core/backend/IAudioBackend.h`
  - `bool open(...)` → `std::expected<void, ao::Error> open(...)`
  - Remove `lastError()`

- **[MODIFY]** `app/platform/linux/playback/PipeWireBackend.h` / `.cpp`
- **[MODIFY]** `app/platform/linux/playback/AlsaExclusiveBackend.h` / `.cpp`
- **[MODIFY]** `app/core/backend/NullBackend.h`
  - Update implementations. Drop `_lastError` member.
  - Keep `setError()` in PipeWire for the async error callback path, but it only
    feeds `onBackendError` now, not a getter.

#### 1.4 — PCM source interface and implementations

- **[MODIFY]** `app/core/source/IPcmSource.h`
  - `bool seek(...)` → `std::expected<void, ao::Error> seek(...)`
  - Remove `lastError()`

- **[MODIFY]** `app/core/source/StreamingPcmSource.h` / `.cpp`
- **[MODIFY]** `app/core/source/MemoryPcmSource.h` / `.cpp`
  - `bool initialize()` → `std::expected<void, ao::Error> initialize()`
  - Remove `_lastError`, `fail()`, `clearError()` — propagate `expected` directly.

#### 1.5 — Engine integration

- **[MODIFY]** `app/core/playback/PlaybackEngine.h` / `.cpp`
  - `bool openTrack(...)` → `std::expected<void, ao::Error> openTrack(...)`
  - Update callers to use `.has_value()` / `.error().message` instead of `lastError()`.

#### 1.6 — Test updates

- **[MODIFY]** `test/unit/app/PlaybackEngineTest.cpp`
  - Remove `When(Method(mockBackend, lastError))` stubs.
  - Update `open()` mock to return `expected`.

### Phase 2 — Core Library (`lib/utility/`)

- **[MODIFY]** `include/rs/utility/MappedFile.h`
- **[MODIFY]** `lib/utility/MappedFile.cpp`
  - `std::string map(path)` → `std::expected<void, std::string> map(path)`
  - Callers in `FlacDecoderSession::open()` and `AlacDecoderSession::open()` updated
    in Phase 1 already.

### Phase 3 — No changes needed

These subsystems already use the correct pattern for their domain:

- **LMDB layer:** Keep `ao::Exception` — invariant violations.
- **Expression engine:** Keep `ao::Exception` — parse/evaluation errors reported
  at the expression input boundary.
- **Tag parsing:** Keep `ao::Exception` — malformed files caught at import boundary.
- **`std::optional` lookups:** Keep as-is — legitimate absence.

---

## Verification Plan

### Automated Tests

```bash
# Full debug build with sanitizers
./build.sh debug --clean

# Clang-tidy conformance
./build.sh debug --clean --tidy

# Run unit tests
/tmp/build/debug/test/rs_test
```

### Manual Verification

- Verify playback works end-to-end (open file → decode → play → seek → stop).
- Verify error paths: try playing a corrupt file, disconnecting audio device mid-playback.
- Verify YAML import/export still catches exceptions correctly at boundary.
