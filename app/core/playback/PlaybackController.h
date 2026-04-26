// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/playback/PlaybackTypes.h"

#include <memory>

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
  };

} // namespace app::core::playback
