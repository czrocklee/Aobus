// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <memory>

namespace ao::rt
{
  class PlaybackService;
}

namespace ao::winui
{
  /// Dispatcher-confined owner of the separate Soul window. Playback outlives this owner.
  class FullscreenSoulWindow final
  {
  public:
    explicit FullscreenSoulWindow(rt::PlaybackService& playback) noexcept;
    ~FullscreenSoulWindow() noexcept;

    FullscreenSoulWindow(FullscreenSoulWindow const&) = delete;
    FullscreenSoulWindow& operator=(FullscreenSoulWindow const&) = delete;
    FullscreenSoulWindow(FullscreenSoulWindow&&) = delete;
    FullscreenSoulWindow& operator=(FullscreenSoulWindow&&) = delete;

    void show();

  private:
    class State;

    rt::PlaybackService& _playback;
    std::shared_ptr<State> _statePtr;
  };
} // namespace ao::winui
