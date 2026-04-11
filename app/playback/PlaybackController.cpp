// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "PlaybackController.h"

#include <iostream>

#include "NullBackend.h"
#include "PlaybackEngine.h"

#ifdef PIPEWIRE_FOUND
#include "PipeWireBackend.h"
#endif

namespace app::playback
{

  PlaybackController::PlaybackController()
  {
#ifdef PIPEWIRE_FOUND
    _engine = std::make_unique<PlaybackEngine>(std::make_unique<PipeWireBackend>());
#else
    _engine = std::make_unique<PlaybackEngine>(std::make_unique<NullBackend>());
#endif
  }

  PlaybackController::~PlaybackController() = default;

  void PlaybackController::play(TrackPlaybackDescriptor descriptor)
  {
    std::cerr << "[DEBUG] PlaybackController::play: title=" << descriptor.title << ", path=" << descriptor.filePath
              << std::endl;
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

} // namespace app::playback