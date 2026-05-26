// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/CorePrimitives.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/StateTypes.h>

#include <cstdint>
#include <functional>

namespace ao::gtk
{
  /**
   * Abstracted binding source for playback events and state.
   * Used to isolate GTK widgets from direct rt::PlaybackService subscriptions,
   * enabling pure C++ binder testing without GTK.
   */
  struct PlaybackBindingSource
  {
    std::function<rt::PlaybackState const&()> state;
    std::function<rt::Subscription(std::move_only_function<void()>)> onStarted;
    std::function<rt::Subscription(std::move_only_function<void()>)> onPaused;
    std::function<rt::Subscription(std::move_only_function<void()>)> onStopped;
    std::function<rt::Subscription(std::move_only_function<void()>)> onIdle;
    std::function<rt::Subscription(std::move_only_function<void()>)> onPreparing;
    std::function<rt::Subscription(std::move_only_function<void(rt::PlaybackService::SeekUpdate const&)>)> onSeekUpdate;
    std::function<rt::Subscription(std::move_only_function<void(rt::OutputSelection const&)>)> onOutputChanged;
    std::function<rt::Subscription(std::move_only_function<void(rt::PlaybackService::QualityChanged const&)>)>
      onQualityChanged;
    std::function<rt::Subscription(std::move_only_function<void(rt::PlaybackService::ShuffleModeChanged const&)>)>
      onShuffleModeChanged;
    std::function<rt::Subscription(std::move_only_function<void(rt::PlaybackService::RepeatModeChanged const&)>)>
      onRepeatModeChanged;
    std::function<rt::Subscription(std::move_only_function<void(float)>)> onVolumeChanged;

    std::function<void()> pause;
    std::function<void()> resume;
    std::function<void()> stop;
    std::function<void(std::uint32_t, rt::PlaybackService::SeekMode)> seek;
    std::function<void(float)> setVolume;
  };

  /**
   * Helper to create a PlaybackBindingSource backed by a real rt::PlaybackService.
   */
  PlaybackBindingSource makePlaybackBindingSource(rt::PlaybackService& service);
} // namespace ao::gtk
