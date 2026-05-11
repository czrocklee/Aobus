# Audio Module Refactoring — Implementation Plan

> **Status:** Draft
> **Scope:** `include/ao/audio/`, `lib/audio/`, `test/unit/audio/`, `app/runtime/Services.cpp`
> **Principle:** Each phase is independently mergeable. No phase breaks prior phases.

---

## Phase Dependency Graph

```
P1 (QualityAnalyzer) ──┐
                        ├── P3 (Unify Callbacks) ── P4 (IRenderTarget) ── P5 (TrackSession) ── P6 (Negotiation)
P2 (Subscription.h) ───┘
```

P1 and P2 are parallel. P3 needs P2. P4 needs P3. P5 needs P4. P6 needs P4.

---

## Phase 1: Extract `QualityAnalyzer` from `Player`

**Risk:** 🟢 Low | **Effort:** Small | **LOC:** +90 new / −200 from Player.cpp

### Rationale

`analyzeAudioQuality()`, `findPlaybackPath()`, `assessNodeQuality()`, `processInputSources()` are ~200 lines of pure graph-analysis in `Player::Impl`. They only read `mergedGraph` and write `quality` + `qualityTooltip`. Extracting makes them independently testable and shrinks Player.cpp from 707→~500 lines.

### New Files

**`include/ao/audio/QualityAnalyzer.h`**
```cpp
#pragma once
#include <ao/audio/Backend.h>
#include <ao/audio/flow/Graph.h>
#include <string>

namespace ao::audio
{
  struct QualityResult final
  {
    Quality quality = Quality::Unknown;
    std::string tooltip;
    bool operator==(QualityResult const&) const = default;
  };

  /// Pure function: graph in → quality verdict out.
  QualityResult analyzeAudioQuality(flow::Graph const& graph);
}
```

**`lib/audio/QualityAnalyzer.cpp`** — Move the 4 methods from Player::Impl as free functions.

### Modified Files

| File | Change |
|---|---|
| `lib/audio/Player.cpp` | Remove 4 methods. In `updateMergedGraph()`: `auto const r = analyzeAudioQuality(mergedGraph); quality = r.quality; qualityTooltip = r.tooltip;` |
| `lib/audio/CMakeLists.txt` | Add `QualityAnalyzer.cpp` |

### New Tests

**`test/unit/audio/QualityAnalyzerTest.cpp`** — Migrate the 6 quality SECTIONs from PlayerTest into pure graph-fixture tests. The existing PlayerTest quality tests become redundant (mark `[.integration]` or delete).

---

## Phase 2: Extract `Subscription` to Own Header

**Risk:** 🟢 Trivial | **Effort:** Trivial | **LOC:** ±0

### Rationale

`Subscription` (a general-purpose RAII handle) is defined in `IBackendProvider.h`. Any user transitively includes Backend.h, IBackend.h, etc. Moving it to its own header reduces coupling.

### File Changes

**New:** `include/ao/audio/Subscription.h` — Move lines 19–61 of IBackendProvider.h here.

**Modified:** `include/ao/audio/IBackendProvider.h` — Replace the class with `#include <ao/audio/Subscription.h>`. Existing consumers unaffected.

---

## Phase 3: Unify Callback Registration → `Subscription`

**Risk:** 🟡 Medium | **Effort:** Medium | **LOC:** +30/−20

### Rationale

Three callback patterns exist:
- `setTrackEndedCallback(fn)` — Player (Services.cpp:109)
- `setOnDevicesChanged(fn)` — Player (Services.cpp:120)
- `setOnQualityChanged(fn)` — Player (Services.cpp:131)
- `setOnTrackEnded(fn)` — Engine (Player.cpp:460)
- `setOnRouteChanged(fn)` — Engine (Player.cpp:469)
- `subscribeDevices(fn) → Subscription` — IBackendProvider

Unifying to `Subscription` return gives RAII lifetime control.

### API Changes

**`include/ao/audio/Player.h`**
```diff
-void setTrackEndedCallback(std::function<void()> callback);
-void setOnDevicesChanged(std::function<void(...)> callback);
-void setOnQualityChanged(std::function<void(...)> callback);
+Subscription onTrackEnded(std::function<void()> callback);
+Subscription onDevicesChanged(std::function<void(...)> callback);
+Subscription onQualityChanged(std::function<void(...)> callback);
```

**`include/ao/audio/Engine.h`**
```diff
-void setOnTrackEnded(std::function<void()> callback);
-void setOnRouteChanged(OnRouteChanged callback);
+Subscription onTrackEnded(std::function<void()> callback);
+Subscription onRouteChanged(OnRouteChanged callback);
```

### Implementation Pattern

```cpp
Subscription Player::onTrackEnded(std::function<void()> callback)
{
  _impl->onTrackEnded = std::move(callback);
  return Subscription{[this]() { _impl->onTrackEnded = nullptr; }};
}
```

### Consumer Migration

**`app/runtime/Services.cpp`** — Store `Subscription` fields in `PlaybackService::Impl`. Replace `player->setTrackEndedCallback(...)` with `trackEndedSub = player->onTrackEnded(...)`.

**`lib/audio/Player.cpp`** — Store subscriptions for Engine callbacks. `Player::~Player()` becomes `= default` (RAII cleanup).

**`test/unit/audio/PlaybackEngineTest.cpp:428`** — `engine.setOnTrackEnded(...)` → `auto sub = engine.onTrackEnded(...)`.

---

## Phase 4: Replace `RenderCallbacks` with `IRenderTarget`

**Risk:** 🟡 Medium | **Effort:** Medium-Large | **LOC:** +40/−80

### Rationale

`RenderCallbacks` is 9 function pointers + `void* userData`. Engine::Impl has 10 static trampoline functions that just cast `userData`. Virtual dispatch has identical cost to indirect function-pointer call — no RT penalty.

### New File

**`include/ao/audio/IRenderTarget.h`**
```cpp
#pragma once
#include <ao/audio/Format.h>
#include <ao/audio/Property.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace ao::audio
{
  class IRenderTarget
  {
  public:
    virtual ~IRenderTarget() = default;
    virtual std::size_t readPcm(std::span<std::byte> output) noexcept = 0;
    virtual bool isSourceDrained() noexcept = 0;
    virtual void onUnderrun() noexcept = 0;
    virtual void onPositionAdvanced(std::uint32_t frames) noexcept = 0;
    virtual void onDrainComplete() noexcept = 0;
    virtual void onRouteReady(std::string_view routeAnchor) noexcept = 0;
    virtual void onFormatChanged(Format const& format) noexcept = 0;
    virtual void onPropertyChanged(PropertyId id) noexcept = 0;
    virtual void onBackendError(std::string_view message) noexcept = 0;
  };
}
```

### API Change

**`include/ao/audio/IBackend.h`** — Delete `RenderCallbacks`. Change:
```diff
-virtual Result<> open(Format const& format, RenderCallbacks callbacks) = 0;
+virtual Result<> open(Format const& format, IRenderTarget& target) = 0;
```

### Transformation Summary

| Component | Before | After |
|---|---|---|
| Engine::Impl | 10 static trampolines + `RenderCallbacks` field | Implements `IRenderTarget` directly |
| PipeWireBackend::Impl | `_callbacks.readPcm(_callbacks.userData, ...)` | `_target->readPcm(...)` |
| AlsaExclusiveBackend::Impl | Same pattern | Same fix |
| CapturingBackend (test) | Stores `RenderCallbacks`, fire methods use `_callbacks.onX(_callbacks.userData, ...)` | Stores `IRenderTarget*`, fire methods use `_target->onX(...)` |
| NullBackend | `open(Format, RenderCallbacks)` | `open(Format, IRenderTarget&)` |

### Files Touched (9)

`IBackend.h`, `IRenderTarget.h` (new), `NullBackend.h`, `AlsaExclusiveBackend.h/.cpp`, `PipeWireBackend.h/.cpp`, `Engine.cpp`, `CapturingBackend.h`, `TestUtility.h`

---

## Phase 5: Extract `TrackSession` from `Engine`

**Risk:** 🔴 High | **Effort:** Large | **LOC:** +250/−300

### Rationale

Engine::Impl (~470 lines) manages 7 responsibilities. `TrackSession` isolates track-specific state (source, position, drain) from engine-level state (backend lifecycle, volume, route).

### New Type

**`include/ao/audio/detail/TrackSession.h`**
```cpp
namespace ao::audio::detail
{
  class TrackSession final
  {
  public:
    TrackSession(std::shared_ptr<ISource> source, DecodedStreamInfo info, std::uint32_t durationMs);

    std::shared_ptr<ISource> source() const noexcept;
    void clearSource() noexcept;

    void advanceFrames(std::uint32_t frames) noexcept;
    void resetPosition(std::uint32_t positionMs) noexcept;
    std::uint32_t positionMs() const noexcept;
    std::uint32_t durationMs() const noexcept;
    std::uint32_t bufferedMs() const noexcept;

    void markDrainPending() noexcept;
    bool consumeDrainPending() noexcept;

    DecodedStreamInfo const& streamInfo() const noexcept;
    std::uint32_t sampleRate() const noexcept;

  private:
    std::atomic<std::shared_ptr<ISource>> _source;
    DecodedStreamInfo _streamInfo;
    std::uint32_t _durationMs;
    std::atomic<std::uint64_t> _accumulatedFrames{0};
    std::atomic<bool> _drainPending{false};
  };
}
```

### Engine::Impl Transformation

Before: `source`, `accumulatedFrames`, `engineSampleRate`, `playbackDrainPending` as separate atomics.

After: `std::unique_ptr<detail::TrackSession> activeSession` replaces all of them.

Also extract `openTrack()`, `negotiateFormat()`, `createPcmSource()` into:
```cpp
namespace ao::audio::detail
{
  struct TrackOpenResult { std::unique_ptr<TrackSession> session; Format backendFormat; std::string errorText; };
  TrackOpenResult openTrack(TrackPlaybackDescriptor const&, IBackend&, Device const&, DecoderFactoryFn const&);
}
```

### New Tests

**`test/unit/audio/TrackSessionTest.cpp`** — Position tracking, drain FSM, source lifecycle.

---

## Phase 6: Push Format Negotiation into Backends

**Risk:** 🟡 Medium | **Effort:** Small | **LOC:** +20/−10

### Rationale

Engine.cpp:327 hardcodes: `if (backendId == kBackendPipeWire && profileId == kProfileShared) { passthrough; }`. This breaks the abstraction — adding a new backend requires editing Engine.

### Solution

**`include/ao/audio/IBackend.h`** — Add default virtual:
```cpp
virtual Format negotiateInputFormat(Format sourceFormat, DeviceCapabilities const& caps) const;
```

Default implementation calls `FormatNegotiator::buildPlan()`.

**`PipeWireBackend`** overrides to return `sourceFormat` in shared mode.

**`Engine.cpp`** — Remove the `if (kBackendPipeWire)` branch:
```diff
-if (backendId == kBackendPipeWire && ...) { ... }
-auto const plan = FormatNegotiator::buildPlan(...);
+backendFormat = backend->negotiateInputFormat(info.sourceFormat, currentDevice.capabilities);
```

---

## Summary Table

| Phase | Description | Risk | Files | New Files | New Tests |
|---|---|---|---|---|---|
| 1 | Extract `QualityAnalyzer` | 🟢 Low | 3 | 2 | 1 |
| 2 | Extract `Subscription.h` | 🟢 Trivial | 1 | 1 | 0 |
| 3 | Unify callbacks → `Subscription` | 🟡 Med | 5 | 0 | 0 |
| 4 | `RenderCallbacks` → `IRenderTarget` | 🟡 Med | 9 | 1 | 0 |
| 5 | Extract `TrackSession` | 🔴 High | 3 | 2 | 1 |
| 6 | Push negotiation into backends | 🟡 Med | 4 | 0 | 0 |
| **Total** | | | **~15** | **6** | **2** |
