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
    void pause();
    void resume();
    void stop();
    void seek(std::uint32_t positionMs);

    PlaybackSnapshot snapshot() const;

  private:
    std::unique_ptr<PlaybackEngine> _engine;
  };

} // namespace app::core::playback
