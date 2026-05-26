// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/PlaybackBindingSource.h"

#include <ao/rt/PlaybackService.h>
#include <ao/rt/StateTypes.h>

namespace ao::gtk
{
  PlaybackBindingSource makePlaybackBindingSource(rt::PlaybackService& service)
  {
    return PlaybackBindingSource{
      .state = [&service] -> rt::PlaybackState const& { return service.state(); },
      .onStarted = [&service](auto handler) { return service.onStarted(std::move(handler)); },
      .onPaused = [&service](auto handler) { return service.onPaused(std::move(handler)); },
      .onStopped = [&service](auto handler) { return service.onStopped(std::move(handler)); },
      .onIdle = [&service](auto handler) { return service.onIdle(std::move(handler)); },
      .onPreparing = [&service](auto handler) { return service.onPreparing(std::move(handler)); },
      .onSeekUpdate = [&service](auto handler) { return service.onSeekUpdate(std::move(handler)); },
      .onOutputChanged = [&service](auto handler) { return service.onOutputChanged(std::move(handler)); },
      .onQualityChanged = [&service](auto handler) { return service.onQualityChanged(std::move(handler)); },
      .onShuffleModeChanged = [&service](auto handler) { return service.onShuffleModeChanged(std::move(handler)); },
      .onRepeatModeChanged = [&service](auto handler) { return service.onRepeatModeChanged(std::move(handler)); },
      .onVolumeChanged = [&service](auto handler) { return service.onVolumeChanged(std::move(handler)); },

      .pause = [&service] { service.pause(); },
      .resume = [&service] { service.resume(); },
      .stop = [&service] { service.stop(); },
      .seek = [&service](auto pos, auto mode) { service.seek(pos, mode); },
      .setVolume = [&service](float vol) { service.setVolume(vol); }};
  }
} // namespace ao::gtk
