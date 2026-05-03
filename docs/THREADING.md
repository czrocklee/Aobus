# Aobus Threading Model

## Why audio threading is different

Audio hardware demands data on a fixed schedule. At 44.1 kHz with a 256-frame period, the callback fires every **5.8 ms**. If that callback is late, the hardware buffer empties and the user hears a glitch.

This imposes one non-negotiable rule:

> **The audio I/O thread must never wait on anything the UI thread might hold.**

The rest of the design follows from this rule.

---

## Thread inventory

### GTK main thread

Runs the Glib event loop. All GTK widget operations happen here.

- User input handling (play, pause, seek, device switching)
- 100 ms timer polls playback state and refreshes the UI
- Executes callbacks dispatched from other threads via `IMainThreadDispatcher`

### Audio I/O thread

ALSA uses a dedicated `std::jthread` (`AlsaPlayback`). PipeWire uses its own `pw_thread_loop`. This is the hot path.

- Reads PCM from `ISource` each period and writes to hardware
- Reports position advances, drain completion, format changes, and errors back to the engine

**Constraints:**
- Never acquires `_stateMutex` — that lock belongs to the UI thread
- Never touches GTK widgets — all UI updates go through dispatch

### Decode thread

`std::jthread` named `StreamingSource-Decode`.

- Calls decoder (`readNextBlock`) to produce PCM blocks
- Writes into `PcmRingBuffer` as the sole producer
- Checks `stop_token` for thread exit and seek cancellation

Decoding runs on a separate thread so it can buffer ahead to the high-water mark without blocking the audio thread.

### Backend monitor threads

- `AlsaDeviceMonitor` — udev poll for device hotplug
- PipeWire monitor — `pw_thread_loop` for graph change notifications

Both detect changes and dispatch them to the UI thread.

### Background worker threads

- `FileImport` — directory scan and file import
- `LibraryExport` — YAML export
- `LibraryImport` — YAML import

Heavy work runs on these threads. Progress and completion are dispatched to the UI thread. Each is guarded with `joinable()` to prevent overlapping runs.

---

## Principles

### 1. The audio thread uses atomics, never `_stateMutex`

`Engine::_stateMutex` protects playback state, current track, and callback pointers. It is acquired by the UI thread during `play()`, `pause()`, `stop()`, `seek()`, `status()`, and `setBackend()`.

The audio thread accesses these via atomics instead:

| Data | Type | Audio thread usage |
|------|------|-------------------|
| PCM source | `atomic<shared_ptr<ISource>>` | `load(acquire)` → `read()` |
| Playback position | `atomic<uint32_t>` | `fetch_add` per period |
| Engine sample rate | `atomic<uint32_t>` | `load(relaxed)` for position math |
| Underrun count | `atomic<uint32_t>` | `++` |
| Backend started | `atomic<bool>` | `load` |
| Drain pending | `atomic<bool>` | `exchange` |

### 2. No mutex between the decode thread and the audio thread

`PcmRingBuffer` uses `boost::lockfree::spsc_queue<std::byte>` — a wait-free single-producer single-consumer queue. No external lock is needed.

| Operation | Caller | Thread |
|-----------|--------|--------|
| `write()` | `StreamingSource::writeBlock` | Decode |
| `read()` | `StreamingSource::read` → `Engine::onReadPcm` | Audio I/O |
| `clear()` | `StreamingSource::seek` | UI (audio stopped) |

This is strictly SPSC. `clear()` runs only when the audio thread has been stopped.

`MemorySource` follows the same pattern: `_pcmBytes` is immutable after `initialize()`, so `_readOffset` is a plain `atomic<size_t>` with `load` / `store` — no mutex.

### 3. All UI hops go through a single dispatch interface

```cpp
class IMainThreadDispatcher {
public:
    virtual ~IMainThreadDispatcher() = default;
    virtual void dispatch(std::function<void()> task) = 0;
};
```

`GtkMainThreadDispatcher` implements it with `Glib::Dispatcher` and a mutex-protected task queue. The dispatcher swaps the queue under lock, then executes every task on the UI thread.

Every non-UI thread — audio callbacks, backend monitors, import/export workers — uses this same entry point. The core audio library never depends on GTK directly.

**Lambda safety:** dispatched lambdas capture data by value, not raw `this` pointers.

```cpp
// Correct — self-contained, no lifetime dependency
_dispatcher->dispatch([cb = _onRouteChanged, snap = _routeStatus]() {
    cb(snap);
});
```

### 4. Callbacks are never invoked while holding an internal lock

Callbacks can do anything — dispatch to UI, acquire other locks, call back into the backend API. Running them under a lock invites reentrancy and deadlock.

The pattern:

```cpp
// Phase 1: build data under lock
std::vector<Callback> pending;
{
    auto lock = std::lock_guard{mutex};
    // ... collect callbacks and snapshot data ...
}

// Phase 2: invoke outside lock
for (auto& cb : pending) {
    cb(data);
}
```

This applies to `PipeWireMonitor::refresh()`, `Engine::handleRouteReady()`, `Engine::handleFormatChanged()`, and `Engine::handleDrainComplete()`.

---

## Seek cancellation

When the user seeks, any in-progress decode work for the old position must stop. `StreamingSource` uses `std::stop_source`:

```cpp
// seek()
_seekStopSource.request_stop();         // old token flips to stop_requested
_seekStopSource = std::stop_source{};   // fresh source
auto token = _seekStopSource.get_token();

// decode loop — checks two tokens
while (!threadStopToken.stop_requested() &&
       !seekToken.stop_requested() &&
       !eof && !failed) {
    decodeNextBlock(seekToken, &threadStopToken);
}
```

`decodeLoop` holds two tokens: the jthread's (thread exit) and the seek source's (work invalidation). `fillUntil` and `decodeNextBlock` use the seek token for fast-exit checks before acquiring `_decoderMutex`.

---

## Subscription lifecycle

```cpp
class Subscription {
    std::move_only_function<void()> _unsub;
public:
    ~Subscription() {
        if (_unsub) _unsub();          // RAII: unsubscribe on destruction
    }

    void reset() {
        if (_unsub) {
            auto f = std::move(_unsub); // move out
            f();                         // execute
            // both f and _unsub are now empty — destructor is a no-op
        }
    }
};
```

`reset()` and destruction both execute the unsubscribe callback exactly once. Moving the callable out before invoking prevents double-unsubscribe.

---

## Mutex inventory

| Mutex | Guards | Held by |
|-------|--------|---------|
| `Engine::_stateMutex` | `_status`, `_currentTrack`, `_routeStatus`, callback pointers | UI thread only |
| `StreamingSource::_decoderMutex` | `_decoder->readNextBlock()` / `_decoder->seek()` | Decode thread + UI thread (seek) |
| `AlsaProvider::Impl::mutex` | Device cache + subscriber list | Monitor thread + UI thread |
| `PipeWireMonitor::mutex` | Graph data + subscription list | pw_thread_loop only (callbacks outside) |
| `GtkMainThreadDispatcher::_mutex` | Task queue | UI thread (swap) + any caller (push) |

No `recursive_mutex` anywhere.

---

## When to use what

**`std::atomic`** — single value, no multi-field atomicity required. Counters, position pointers, state flags.

**`std::mutex`** — multiple fields must be updated together, or protecting a non-thread-safe external resource (e.g., decoder session).

**Minimize scope:**

```cpp
if (token.stop_requested()) return false;   // fast exit before lock

{
    auto lock = std::lock_guard{_decoderMutex};
    auto result = _decoder->readNextBlock(); // lock only where needed
}
```

---

## New code checklist

- [ ] Which thread am I on?
- [ ] If the audio thread — am I waiting on any lock? Never. Use atomics or dispatch.
- [ ] If a non-UI thread — am I updating the UI through `IMainThreadDispatcher::dispatch()`?
- [ ] If I invoke a callback — could it re-enter the lock I'm holding? Move it outside.
- [ ] If I assign a `std::jthread` member — could the old thread still be running? Check `joinable()`.
- [ ] If I capture `this` in a dispatched lambda — is the lifetime guaranteed? Prefer value captures.
