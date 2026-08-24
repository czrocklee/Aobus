// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/audio/BackendProvider.h>

#include <memory>
#include <vector>

namespace ao::audio
{
  /**
   * @brief macOS has no native backend provider yet.
   *
   * Returning an empty provider list is a supported state, not a failure.
   * Player starts every engine on NullBackend and only adopts a real device
   * once a provider publishes one, so the application runs, browses the
   * library, and reports playback progress while staying silent.
   *
   * A CoreAudio provider replaces the empty list here and needs no change
   * anywhere else in the audio graph.
   */
  std::vector<std::unique_ptr<BackendProvider>> createPlatformBackendProviders()
  {
    return {};
  }
} // namespace ao::audio
