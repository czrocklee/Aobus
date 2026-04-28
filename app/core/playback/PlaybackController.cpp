// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "core/playback/PlaybackController.h"
#include "core/Log.h"
#include "core/backend/IDeviceDiscovery.h"
#include "core/backend/NullBackend.h"
#include "core/playback/PlaybackEngine.h"

#include <algorithm>
#include <ranges>
#include <map>

namespace app::core::playback
{
  PlaybackController::PlaybackController(std::shared_ptr<IMainThreadDispatcher> dispatcher)
    : _dispatcher(std::move(dispatcher))
  {
    // Start with a NullBackend until a discovery provides something real
    _engine = std::make_unique<PlaybackEngine>(std::make_unique<backend::NullBackend>(),
                                               backend::AudioDevice{
                                                 .id = "null",
                                                 .displayName = "None",
                                                 .description = "No audio output selected",
                                                 .backendKind = backend::BackendKind::None,
                                                 .capabilities = {}
                                               },
                                               _dispatcher);

    _engine->setOnTrackEnded([this]() {
      if (_onTrackEnded) _onTrackEnded();
    });
  }

  void PlaybackController::setTrackEndedCallback(std::function<void()> callback)
  {
    _onTrackEnded = std::move(callback);
  }

  PlaybackController::~PlaybackController() = default;

  void PlaybackController::addDiscovery(std::unique_ptr<backend::IDeviceDiscovery> discovery)
  {
    if (!discovery) return;

    discovery->setDevicesChangedCallback([this] { _backendsDirty = true; });
    _discoveries.push_back(std::move(discovery));
    _backendsDirty = true;
  }

  void PlaybackController::play(TrackPlaybackDescriptor descriptor)
  {
    _engine->play(descriptor);
  }

  void PlaybackController::setOutput(backend::BackendKind kind, std::string_view deviceId)
  {
    auto const currentSnap = _engine->snapshot();

    // 1. Check if we already have this output active
    if (kind == currentSnap.backend && deviceId == currentSnap.currentDeviceId)
    {
      return;
    }

    if (_allDevices.empty())
    {
      snapshot();
    }

    // 2. Find the AudioDevice matching the kind and id from our cache
    auto const it = std::ranges::find_if(
      _allDevices, [&](backend::AudioDevice const& d) { return d.backendKind == kind && d.id == deviceId; });

    if (it == _allDevices.end())
    {
      PLAYBACK_LOG_ERROR("PlaybackController: Requested unknown output {}:{}", backendKindToId(kind), deviceId);
      return;
    }

    auto const& targetDevice = *it;

    // 3. Find the discovery object that can handle this BackendKind
    for (auto const& discovery : _discoveries)
    {
      auto devices = discovery->enumerateDevices();
      auto const found = std::ranges::any_of(devices, [&](backend::AudioDevice const& d) { return d == targetDevice; });

      if (found)
      {
        auto backend = discovery->createBackend(targetDevice);
        if (backend)
        {
          _engine->setBackend(std::move(backend), targetDevice);
          return;
        }
      }
    }

    PLAYBACK_LOG_ERROR(
      "PlaybackController: Failed to create backend for output {}:{}", backendKindToId(kind), deviceId);
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
    auto snap = _engine->snapshot();

    if (_backendsDirty.exchange(false))
    {
      auto allDevices = std::vector<backend::AudioDevice>{};
      for (auto const& discovery : _discoveries)
      {
        auto devices = discovery->enumerateDevices();
        allDevices.insert(
          allDevices.end(), std::make_move_iterator(devices.begin()), std::make_move_iterator(devices.end()));
      }

      // Group devices by BackendKind
      std::map<backend::BackendKind, std::vector<backend::AudioDevice>> groups;
      for (auto const& d : allDevices)
      {
        groups[d.backendKind].push_back(d);
      }

      auto snapshots = std::vector<BackendSnapshot>{};
      for (auto& [kind, devices] : groups)
      {
        snapshots.push_back({.kind = kind,
                             .displayName = std::string(backendDisplayName(kind)),
                             .shortName = std::string(backendShortName(kind)),
                             .id = std::string(backendKindToId(kind)),
                             .devices = std::move(devices)});
      }

      _cachedBackends = std::move(snapshots);
      _allDevices = allDevices; // Store flat list for setOutput lookup
    }

    snap.availableBackends = _cachedBackends;
    return snap;
  }

} // namespace app::core::playback
