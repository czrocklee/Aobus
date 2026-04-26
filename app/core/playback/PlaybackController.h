// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/playback/OutputMenuModel.h"
#include "core/playback/PlaybackTypes.h"

#include <chrono>
#include <memory>
#include <vector>

namespace app::core::playback
{
  class PlaybackEngine;
  class IAudioBackend;
}

namespace app::core::playback
{

  class PlaybackController final
  {
  public:
    PlaybackController();
    ~PlaybackController();

    void play(TrackPlaybackDescriptor descriptor);
    void setBackend(std::unique_ptr<IAudioBackend> backend);
    void setDevice(std::string_view deviceId);
    void setBackendAndDevice(std::unique_ptr<IAudioBackend> backend, std::string_view deviceId);
    void setOutput(BackendKind kind, std::string_view deviceId);
    void pause();
    void resume();
    void stop();
    void seek(std::uint32_t positionMs);

    PlaybackSnapshot snapshot() const;

  private:
    std::unique_ptr<PlaybackEngine> _engine;

    // Persistent backends for device enumeration even when inactive
    std::unique_ptr<IAudioBackend> _pwDiscovery;
    std::unique_ptr<IAudioBackend> _alsaDiscovery;

    // Caching for available backends/devices to throttle UI polling
    mutable std::vector<BackendSnapshot> _cachedBackends;
    mutable std::chrono::steady_clock::time_point _lastDiscoveryTime;
  };

} // namespace app::core::playback
