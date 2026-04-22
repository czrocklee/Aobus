// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "core/playback/PlaybackController.h"

#include "core/playback/NullBackend.h"
#include "core/playback/PlaybackEngine.h"

#ifdef PIPEWIRE_FOUND
#include "platform/linux/playback/PipeWireBackend.h"
#endif

namespace app::core::playback
{

  PlaybackController::PlaybackController()
  {
#ifdef PIPEWIRE_FOUND
    _engine = std::make_unique<PlaybackEngine>(std::make_unique<::app::playback::PipeWireBackend>());
#else
    _engine = std::make_unique<PlaybackEngine>(std::make_unique<NullBackend>());
#endif
  }

  PlaybackController::~PlaybackController() = default;

  void PlaybackController::play(TrackPlaybackDescriptor descriptor)
  {
    _engine->play(descriptor);
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
    return _engine->snapshot();
  }

} // namespace app::core::playback
