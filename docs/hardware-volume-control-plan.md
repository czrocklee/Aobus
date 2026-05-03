# Hardware Volume Control Implementation Plan

> Audited and revised 2026-05-04. Original plan by Yang Li; audit against current `main` branch (`68ef21b`).

## 0. Audit Summary

The original plan correctly identifies the four layers (Interface, ALSA, PipeWire, Engine/UI) and the core APIs (`snd_mixer_t`, `pw_stream_set_control`). However it misses several architectural realities of the current codebase:

| Gap | Impact |
|-----|--------|
| IBackend has **zero** volume methods — nothing to "uncomment" | Phase 1 is net-new API design, not cleanup |
| ALSA backend has **no card-info query** during `open()` | Must add `snd_pcm_info` before mixer attach |
| PipeWire `pw_stream_set_control` requires **thread-loop lock** | Every setVolume/setMute call must lock `_threadLoop` |
| PlaybackBar has **no volume slider** — `_seekScale` is track position | VolumeBar is a new widget, not a replacement |
| Player, Engine, PlaybackCoordinator have **no volume state or methods** | Three new delegation layers needed |
| `PlaybackCoordinator` polls at 100ms via `g_timeout_add` | Volume state rides this existing timer — no new polling |
| No `isVolumeAvailable()` → UI hide/show design | Must decide: hide or grey out when unavailable |
| Exclusive-mode volume semantics differ per backend | Document: ALSA = hardware mixer gain; PipeWire exclusive = may be unavailable |

**Revised scope:** 8 phases, ~12 files touched, 2 new files (`VolumeBar.h`/`.cpp`).

---

## 1. Interface Layer (`IBackend.h`)

### 1.1 New pure-virtual methods on `IBackend`

```cpp
// After line 80 (after isExclusiveMode), before backendId:

virtual void setVolume(float volume) = 0;
virtual float getVolume() const = 0;
virtual void setMuted(bool muted) = 0;
virtual bool isMuted() const = 0;
virtual bool isVolumeAvailable() const noexcept = 0;
```

- `volume` is always `[0.0f, 1.0f]` — linear normalised range. Backend maps to hardware.
- `isVolumeAvailable()` lets the UI decide visibility. False on DACs without hardware mixer, or PipeWire exclusive without stream support.
- Return types deliberately simple — mixer ioctls are fire-and-forget; errors are logged, not propagated.

### 1.2 New callback in `RenderCallbacks`

```cpp
// After onBackendError (line 45), add:
void (*onVolumeChanged)(void* userData) noexcept = nullptr;
```

This fires when the *system* changes volume externally (e.g. `alsamixer`, `pavucontrol`). The backend is responsible for detecting this and notifying Engine.

---

## 2. ALSA Exclusive Backend (`AlsaExclusiveBackend.cpp`)

### 2.1 Impl structure additions

```cpp
struct AlsaExclusiveBackend::Impl final {
    // … existing members (pcm, thread, paused, etc.) …

    // --- Mixer members ---
    struct AlsaMixerDeleter {
        void operator()(::snd_mixer_t* h) const noexcept {
            if (h) ::snd_mixer_close(h);
        }
    };
    using AlsaMixerPtr = std::unique_ptr<::snd_mixer_t, AlsaMixerDeleter>;

    AlsaMixerPtr       mixer;
    ::snd_mixer_elem_t* mixerElem = nullptr;   // non-owning
    std::string        mixerElemName;          // debug/log
    long               volMin = 0, volMax = 100;
    bool               hasDB   = false;
    long               dbMin = 0, dbMax = 0;

    // Methods
    bool initMixer(::snd_pcm_t* pcm);
    void applyVolume(float vol);
    void applyMute(bool mute);
    float readVolume() const;
    bool readMute() const;
};
```

### 2.2 Mixer initialisation (`initMixer`)

Called at the end of `open()`, right after `_impl->pcm = std::move(safePcm)` (line 365):

```cpp
bool AlsaExclusiveBackend::Impl::initMixer(::snd_pcm_t* pcm) {
    ::snd_pcm_info_t* info = nullptr;
    snd_pcm_info_alloca(&info);                          // stack-alloc macro
    if (::snd_pcm_info(pcm, info) < 0) return false;
    int card = ::snd_pcm_info_get_card(info);

    ::snd_mixer_t* raw = nullptr;
    if (::snd_mixer_open(&raw, 0) < 0) return false;
    mixer.reset(raw);

    auto cardStr = std::format("hw:{}", card);
    if (::snd_mixer_attach(raw, cardStr.c_str()) < 0) return false;
    if (::snd_mixer_selem_register(raw, nullptr, nullptr) < 0) return false;
    if (::snd_mixer_load(raw) < 0) return false;

    // Heuristic search — first match wins
    static constexpr const char* kSelemNames[] = {"Master", "PCM", "Digital", "Main"};
    for (auto* name : kSelemNames) {
        auto* elem = ::snd_mixer_first_elem(raw);
        while (elem) {
            if (::snd_mixer_selem_get_name(elem) == std::string_view{name}) {
                mixerElem = elem;
                mixerElemName = name;
                break;
            }
            elem = ::snd_mixer_elem_next(elem);
        }
        if (mixerElem) break;
    }
    if (!mixerElem) return false;

    ::snd_mixer_selem_get_playback_volume_range(mixerElem, &volMin, &volMax);
    hasDB = (::snd_mixer_selem_get_playback_dB_range(mixerElem, &dbMin, &dbMax) == 0);
    return true;
}
```

### 2.3 Volume / mute operations

```cpp
void AlsaExclusiveBackend::Impl::applyVolume(float vol) {
    if (!mixerElem) return;
    float clamped = std::clamp(vol, 0.0f, 1.0f);
    if (hasDB) {
        long db = dbMin + static_cast<long>((dbMax - dbMin) * clamped);
        ::snd_mixer_selem_set_playback_dB_all(mixerElem, db, 0);
    } else {
        long val = volMin + static_cast<long>((volMax - volMin) * clamped);
        ::snd_mixer_selem_set_playback_volume_all(mixerElem, val);
    }
}

void AlsaExclusiveBackend::Impl::applyMute(bool mute) {
    if (!mixerElem) return;
    ::snd_mixer_selem_set_playback_switch_all(mixerElem, mute ? 0 : 1);
}

float AlsaExclusiveBackend::Impl::readVolume() const {
    if (!mixerElem) return 1.0f;
    long val = 0;
    if (hasDB) {
        long db = 0;
        ::snd_mixer_selem_get_playback_dB(mixerElem, SND_MIXER_SCHN_MONO, &db);
        if (dbMax != dbMin) return std::clamp(static_cast<float>(db - dbMin) / (dbMax - dbMin), 0.0f, 1.0f);
    }
    ::snd_mixer_selem_get_playback_volume(mixerElem, SND_MIXER_SCHN_MONO, &val);
    if (volMax != volMin) return std::clamp(static_cast<float>(val - volMin) / (volMax - volMin), 0.0f, 1.0f);
    return 1.0f;
}
```

### 2.4 Thread model

- `setVolume`, `setMuted`, `getVolume`, `isMuted` are called from the UI thread (via Engine → Player → PlaybackCoordinator → 100ms timer).
- ALSA mixer simple API performs lightweight ioctls; these do NOT block on the PCM device and are safe to call from any thread.
- The playback `std::jthread` never touches mixer state.

### 2.5 Interface method implementations

```cpp
void AlsaExclusiveBackend::setVolume(float volume) override  { _impl->applyVolume(volume); }
float AlsaExclusiveBackend::getVolume() const override       { return _impl->readVolume(); }
void AlsaExclusiveBackend::setMuted(bool muted) override     { _impl->applyMute(muted); }
bool AlsaExclusiveBackend::isMuted() const override {
    if (!_impl->mixerElem) return false;
    int val = 0;
    ::snd_mixer_selem_get_playback_switch(_impl->mixerElem, SND_MIXER_SCHN_MONO, &val);
    return val == 0;
}
bool AlsaExclusiveBackend::isVolumeAvailable() const noexcept override {
    return _impl && _impl->mixerElem != nullptr;
}
```

---

## 3. PipeWire Backend (`PipeWireBackend.cpp`)

### 3.1 Impl additions

```cpp
struct PipeWireBackend::Impl final {
    // … existing members …
    std::atomic<float> _volume{1.0f};
    std::atomic<bool>  _muted{false};
    bool _volumeAvailable = false;
};
```

### 3.2 `setVolume` / `setMuted` — thread-loop-safe

Every `pw_stream_set_control` call must hold the thread-loop lock:

```cpp
void PipeWireBackend::setVolume(float vol) {
    float clamped = std::clamp(vol, 0.0f, 1.0f);
    _impl->_volume.store(clamped, std::memory_order_relaxed);
    if (!_impl->_threadLoop || !_impl->_stream) return;

    std::array<std::uint8_t, 128> buf{};
    auto* b = reinterpret_cast<::spa_pod_builder*>(buf.data()); // simplified; actual init needed
    // Build SPA pod with SPA_PROP_volume = clamped
    // … see PipeWire spa/pod/builder.h for exact builder API …

    ::pw_thread_loop_lock(_impl->_threadLoop.get());
    ::pw_stream_set_control(_impl->_stream.get(), SPA_PROP_volume, 1, &pod);
    ::pw_thread_loop_unlock(_impl->_threadLoop.get());
}
```

Same pattern for `setMuted` using `SPA_PROP_mute` (0x10004).

### 3.3 `isVolumeAvailable` semantics

| Profile | Return | Reason |
|---------|--------|--------|
| `kProfileShared` | `true` | PipeWire always supports stream volume in shared mode |
| `kProfileExclusive` | `false` (default), or `true` if proven | With `PW_STREAM_FLAG_NO_CONVERT`, stream volume may not propagate. Default to unavailable until we can test with real hardware. **No ALSA mixer fallback** — avoids contention between PipeWire and ALSA backends controlling the same hardware mixer. |

### 3.4 Read-back via param_changed

If volume/mute is set via `pw_stream_set_control`, PipeWire may echo the change back via `handleStreamParamChanged` with `SPA_PROP_volume` / `SPA_PROP_mute` IDs. Extend `handleStreamParamChanged` to handle these param IDs and fire `onVolumeChanged` when detected.

---

## 4. Engine (`Engine.h` / `Engine.cpp`)

### 4.1 Status struct addition

```cpp
struct Status final {
    // … existing fields …
    float  volume          = 1.0f;
    bool   muted           = false;
    bool   volumeAvailable = false;
};
```

### 4.2 New public methods

```cpp
void setVolume(float vol);
void setMuted(bool muted);
float getVolume() const;
bool isMuted() const;
bool isVolumeAvailable() const;
```

Implementation: forward to `_backend->setVolume(vol)`, etc. After calling, update `_status.volume` / `_status.muted` under `_stateMutex`.

### 4.3 New static callback

```cpp
static void onVolumeChanged(void* userData) noexcept;
```

Wired in `Engine::play()` line 193 alongside other callbacks:
```cpp
callbacks.onVolumeChanged = &Engine::onVolumeChanged;
```

Implementation dispatches to `handleVolumeChanged()`:
```cpp
void Engine::handleVolumeChanged() {
    auto const lock = std::lock_guard{_stateMutex};
    if (_backend) {
        _status.volume = _backend->getVolume();
        _status.muted  = _backend->isMuted();
        _status.volumeAvailable = _backend->isVolumeAvailable();
    }
    // Fire OnRouteChanged to push to Player → UI
    if (_onRouteChanged) {
        auto snap = _routeStatus;
        lock.unlock(); // or copy, then callback outside lock
        _onRouteChanged(snap);
    }
}
```

---

## 5. Player (`Player.h` / `Player.cpp`)

### 5.1 Status struct additions

```cpp
struct Status final {
    // … existing fields …
    float  volume          = 1.0f;
    bool   muted           = false;
    bool   volumeAvailable = false;
};
```

### 5.2 New public methods

```cpp
void setVolume(float vol);
void setMuted(bool muted);
void toggleMute();
```

Implementations delegate to `_impl->engine->setVolume(vol)` etc.

### 5.3 Status synchronisation

In `Player::handleRouteChanged()` (called when Engine fires `_onRouteChanged`), sync the new volume fields from `Engine::Status` into the merged Player status. This ensures the 100ms poll in `PlaybackCoordinator::refreshPlaybackBar()` picks up volume changes.

---

## 6. UI — PlaybackBar & New VolumeBar

### 6.1 PlaybackCoordinator wiring

In `PlaybackCoordinator::setupPlayback()` (line 58), add:

```cpp
_playbackBar->signalVolumeChanged().connect([this](float vol) {
    if (_player) _player->setVolume(vol);
});
_playbackBar->signalMuteToggled().connect([this]() {
    if (_player) _player->toggleMute();
});
```

### 6.2 PlaybackBar signal additions

```cpp
// New signal types in PlaybackBar.h:
using VolumeChangedSignal = sigc::signal<void(float)>;
using MuteToggledSignal   = sigc::signal<void()>;

// New accessors:
VolumeChangedSignal& signalVolumeChanged();
MuteToggledSignal&   signalMuteToggled();

// New members:
Gtk::Scale        _volumeScale;       // Phase 1: standard Gtk::Scale (vertical)
Gtk::ToggleButton _muteButton;
// Future Phase 2: replace _volumeScale with custom VolumeBar widget
```

### 6.3 VolumeBar custom widget (Phase 2)

**New files:** `app/linux-gtk/ui/VolumeBar.h`, `app/linux-gtk/ui/VolumeBar.cpp`

A GTK4 custom widget drawn with Cairo via `snapshot_vfunc`:

```
┌─ Spec ─────────────────────────────────────────┐
│ 12 vertical segments, 2px gap between each      │
│ Height profile (0=bottom, 11=top):              │
│   Segments 0-3:   fill 25% of bar height        │
│   Segments 4-7:   fill 50% of bar height        │
│   Segments 8-10:  fill 75% of bar height        │
│   Segment 11:     fill 100% of bar height       │
│ Active segments:   accent color  (#F97316)      │
│ Inactive segments: muted gray   (#4B5563)       │
│ Rounded rect per segment (2px radius)           │
│ Total bar height = parent allocation height     │
│ Bar width = allocation width                    │
│                                                 │
│ Interaction:                                    │
│   Click: snap volume to click Y position        │
│   Drag:  track pointer motion (GestureDrag)     │
│   Scroll: step ±5% (GtkEventControllerScroll)   │
└─────────────────────────────────────────────────┘
```

**Why `snapshot_vfunc` not CSS:** CSS cannot express 12 independent segment heights with profile-based ramping. Cairo `snapshot_vfunc` provides pixel-perfect control over each rounded rectangle's geometry and fill colour.

### 6.4 PlaybackBar layout (`setupLayout`)

New layout (appended right of transport buttons, left of seek):

```
[Output] [⏮ ⏯ ⏭] [🔇 | VolumeBar] [====seek====] [time]
```

The mute button uses `audio-volume-muted-symbolic` / `audio-volume-high-symbolic` icon names.

### 6.5 `setSnapshot` volume handling

In `PlaybackBar::setSnapshot()`, add:

```cpp
// Hide volume controls when unavailable
bool const volAvailable = status.volumeAvailable;
_muteButton.set_visible(volAvailable);
_volumeScale.set_visible(volAvailable);

if (volAvailable) {
    _volumeScale.set_value(status.volume * 100.0);
    _muteButton.set_active(status.muted);
    // Update mute icon based on muted && volume level
}
```

---

## 7. Build & File Manifest

| Phase | File | Action |
|-------|------|--------|
| P1 | `include/ao/audio/IBackend.h` | Add 5 virtual methods + 1 callback |
| P1 | `lib/audio/Engine.h` | Add 5 methods + 1 static callback + 3 Status fields |
| P1 | `lib/audio/Engine.cpp` | Implement delegation + callback |
| P2 | `lib/audio/backend/AlsaExclusiveBackend.cpp` | Add ~100 lines: RAII deleter, initMixer, apply*, read*, 5 overrides |
| P3 | `lib/audio/backend/PipeWireBackend.cpp` | Add ~60 lines: setVolume/setMuted with lock, param_changed extension |
| P4 | `include/ao/audio/Player.h` | Add 3 methods + 3 Status fields |
| P4 | `lib/audio/Player.cpp` | Implement delegation + status sync |
| P5 | `app/linux-gtk/ui/PlaybackBar.h` | Add 2 signals, 2 members (Gtk::Scale + ToggleButton) |
| P5 | `app/linux-gtk/ui/PlaybackBar.cpp` | Layout, setupSignals, setSnapshot volume handling |
| P5 | `app/linux-gtk/ui/PlaybackCoordinator.cpp` | Wire 2 new signals → Player |
| P6 | `app/linux-gtk/ui/VolumeBar.h` (new) | Custom widget declaration |
| P6 | `app/linux-gtk/ui/VolumeBar.cpp` (new) | snapshot_vfunc Cairo rendering + gesture controllers |
| P6 | `app/linux-gtk/CMakeLists.txt` | Add VolumeBar.cpp to build |

---

## 8. Verification Checklist

- [ ] **Build:** `./build.sh debug` passes with no new warnings
- [ ] **Tests:** `ao_test` — all existing tests pass
- [ ] **ALSA with mixer:** Volume slider changes DAC gain (verify with `alsamixer` in parallel)
- [ ] **ALSA without mixer:** `isVolumeAvailable()` returns false; volume controls hidden in UI
- [ ] **PipeWire shared:** Volume slider changes stream volume (verify with `pw-mon` or `pavucontrol`)
- [ ] **PipeWire exclusive:** `isVolumeAvailable()` returns false or true depending on hardware
- [ ] **Mute toggle:** Mutes/unmutes without affecting volume level memory
- [ ] **External change:** Changing volume in `alsamixer` is reflected in UI (if polling implemented)
- [ ] **Thread safety:** Rapidly drag volume slider during playback — no xruns, no crashes
- [ ] **Regression:** Play, pause, resume, seek, stop, device switch all still work
