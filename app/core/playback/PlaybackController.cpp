// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "core/playback/PlaybackController.h"

#include "core/Log.h"
#include "core/playback/NullBackend.h"
#include "core/playback/PlaybackEngine.h"

#ifdef PIPEWIRE_FOUND
#include "platform/linux/playback/PipeWireBackend.h"
#endif

#ifdef ALSA_FOUND
#include "platform/linux/playback/AlsaExclusiveBackend.h"
#endif

namespace app::core::playback
{

  PlaybackController::PlaybackController()
  {
#ifdef PIPEWIRE_FOUND
    _pwDiscovery = std::make_unique<app::playback::PipeWireBackend>();
#endif
#ifdef ALSA_FOUND
    _alsaDiscovery = std::make_unique<app::playback::AlsaExclusiveBackend>();
#endif

    // Default engine with PipeWire
    auto backend = std::unique_ptr<IAudioBackend>{};
    if (_pwDiscovery)
    {
      backend = std::make_unique<app::playback::PipeWireBackend>();
    }
    else
    {
      backend = std::make_unique<NullBackend>();
    }
    _engine = std::make_unique<PlaybackEngine>(std::move(backend));
  }

  PlaybackController::~PlaybackController() = default;

  void PlaybackController::play(TrackPlaybackDescriptor descriptor)
  {
    _engine->play(descriptor);
  }

  void PlaybackController::setBackend(std::unique_ptr<IAudioBackend> backend)
  {
    _engine->setBackend(std::move(backend));
  }

  void PlaybackController::setDevice(std::string_view deviceId)
  {
    _engine->setDevice(deviceId);
  }

  void PlaybackController::setBackendAndDevice(std::unique_ptr<IAudioBackend> backend, std::string_view deviceId)
  {
    if (backend)
    {
      backend->setDevice(deviceId);
    }
    _engine->setBackend(std::move(backend));
  }

  void PlaybackController::setOutput(BackendKind kind, std::string_view deviceId)
  {
    auto const currentSnap = _engine->snapshot();

    if (kind == currentSnap.backend)
    {
      setDevice(deviceId);
      return;
    }

    auto backend = std::unique_ptr<IAudioBackend>{};
    bool exclusiveMode = false;

    switch (kind)
    {
#ifdef PIPEWIRE_FOUND
      case BackendKind::PipeWire: backend = std::make_unique<app::playback::PipeWireBackend>(); break;
      case BackendKind::PipeWireExclusive:
        backend = std::make_unique<app::playback::PipeWireBackend>();
        exclusiveMode = true;
        break;
#endif
#ifdef ALSA_FOUND
      case BackendKind::AlsaExclusive: backend = std::make_unique<app::playback::AlsaExclusiveBackend>(); break;
#endif
      default: break;
    }

    if (backend)
    {
      if (exclusiveMode)
      {
        backend->setExclusiveMode(true);
      }
      setBackendAndDevice(std::move(backend), deviceId);
    }
  }

  void PlaybackController::pause()
  {
    _engine->pause();
  }

  void PlaybackController::resume()
  {
    _engine->resume();
  }

  void PlaybackController::stop()
  {
    _engine->stop();
  }

  void PlaybackController::seek(std::uint32_t positionMs)
  {
    _engine->seek(positionMs);
  }

  PlaybackSnapshot PlaybackController::snapshot() const
  {
    using namespace std::chrono_literals;

    auto snap = _engine->snapshot();
    auto const now = std::chrono::steady_clock::now();

    if (!_cachedBackends.empty() && (now - _lastDiscoveryTime) < 5s)
    {
      snap.availableBackends = _cachedBackends;
      return snap;
    }

    auto allBackends = std::vector<BackendSnapshot>{};

    // 1. Add PipeWire (shared/mixing mode)
    if (_pwDiscovery)
    {
      auto pwDevices = _pwDiscovery->enumerateDevices();
      PLAYBACK_LOG_DEBUG("PlaybackController: PipeWire discovery found {} devices", pwDevices.size());
      allBackends.push_back({.kind = BackendKind::PipeWire,
                             .displayName = std::string(backendDisplayName(BackendKind::PipeWire)),
                             .shortName = std::string(backendShortName(BackendKind::PipeWire)),
                             .id = std::string(backendKindToId(BackendKind::PipeWire)),
                             .devices = pwDevices});

      // Also report PipeWire Exclusive with the same device list
      allBackends.push_back({.kind = BackendKind::PipeWireExclusive,
                             .displayName = std::string(backendDisplayName(BackendKind::PipeWireExclusive)),
                             .shortName = std::string(backendShortName(BackendKind::PipeWireExclusive)),
                             .id = std::string(backendKindToId(BackendKind::PipeWireExclusive)),
                             .devices = std::move(pwDevices)});
    }

    // 2. Add ALSA Exclusive
    if (_alsaDiscovery)
    {
      auto alsaDevices = _alsaDiscovery->enumerateDevices();
      PLAYBACK_LOG_DEBUG("PlaybackController: ALSA discovery found {} devices", alsaDevices.size());
      allBackends.push_back({.kind = BackendKind::AlsaExclusive,
                             .displayName = std::string(backendDisplayName(BackendKind::AlsaExclusive)),
                             .shortName = std::string(backendShortName(BackendKind::AlsaExclusive)),
                             .id = std::string(backendKindToId(BackendKind::AlsaExclusive)),
                             .devices = std::move(alsaDevices)});
    }

    _cachedBackends = allBackends;
    _lastDiscoveryTime = now;

    snap.availableBackends = std::move(allBackends);
    return snap;
  }

} // namespace app::core::playback
