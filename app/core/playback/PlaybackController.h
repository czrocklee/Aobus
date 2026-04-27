// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/backend/BackendTypes.h"
#include "core/playback/PlaybackTypes.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

namespace app::core::backend
{
  class IDeviceDiscovery;
}

namespace app::core::playback
{
  class PlaybackEngine;

  class PlaybackController final
  {
  public:
    PlaybackController();
    ~PlaybackController();

    void addDiscovery(std::unique_ptr<backend::IDeviceDiscovery> discovery);

    void play(TrackPlaybackDescriptor descriptor);
    void setOutput(backend::BackendKind kind, std::string_view deviceId);
    void pause();
    void resume();
    void stop();
    void seek(std::uint32_t positionMs);

    PlaybackSnapshot snapshot() const;

  private:
    std::unique_ptr<PlaybackEngine> _engine;

    // Discovery monitors
    std::vector<std::unique_ptr<backend::IDeviceDiscovery>> _discoveries;

    mutable std::atomic<bool> _backendsDirty{true};
    mutable std::vector<BackendSnapshot> _cachedBackends;
    mutable std::vector<backend::AudioDevice> _allDevices;
  };

} // namespace app::core::playback