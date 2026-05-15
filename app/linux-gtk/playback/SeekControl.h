// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "playback/PlaybackPositionInterpolator.h"
#include "runtime/PlaybackService.h"
#include <gtkmm/scale.h>

#include <cstdint>

namespace ao::gtk
{
  /**
   * @brief A composite widget providing a seek slider for playback.
   */
  class SeekControl final
  {
  public:
    explicit SeekControl(rt::PlaybackService& playbackService);
    ~SeekControl() = default;

    SeekControl(SeekControl const&) = delete;
    SeekControl& operator=(SeekControl const&) = delete;
    SeekControl(SeekControl&&) = delete;
    SeekControl& operator=(SeekControl&&) = delete;

    Gtk::Widget& widget() { return _scale; }

  private:
    void reset();

    rt::PlaybackService& _playbackService;
    Gtk::Scale _scale;
    PlaybackPositionInterpolator _interpolator;
    std::uint32_t _durationMs = 0;
    bool _isDragging = false;
    bool _updating = false;

    rt::Subscription _startedSub;
    rt::Subscription _pausedSub;
    rt::Subscription _idleSub;
    rt::Subscription _stoppedSub;
    rt::Subscription _preparingSub;
  };
} // namespace ao::gtk
