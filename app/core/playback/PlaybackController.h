// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/playback/OutputMenuModel.h"
#include "core/playback/PlaybackTypes.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

namespace app::core::playback
{
  class PlaybackEngine;
  class IAudioBackend;
  class IDeviceDiscovery;
}

namespace app::core::playback
{

  class PlaybackController final
  {
  public:
    PlaybackController();
    ~PlaybackController();

    void addDiscovery(std::unique_ptr<IDeviceDiscovery> discovery);

    void play(TrackPlaybackDescriptor descriptor);
    void setOutput(BackendKind kind, std::string_view deviceId);
    void pause();
    void resume();
    void stop();
    void seek(std::uint32_t positionMs);

    PlaybackSnapshot snapshot() const;

  private:
    std::unique_ptr<PlaybackEngine> _engine;

    // Discovery monitors
    std::vector<std::unique_ptr<IDeviceDiscovery>> _discoveries;

    mutable std::atomic<bool> _backendsDirty{true};
    mutable std::vector<BackendSnapshot> _cachedBackends;
    mutable std::vector<AudioDevice> _allDevices;
  };

} // namespace app::core::playback
