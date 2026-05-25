// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "playback/PlaybackPositionInterpolator.h"
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/PlaybackService.h>

#include <gtkmm/label.h>
#include <gtkmm/widget.h>

#include <cstdint>

namespace ao::gtk
{
  enum class TimeLabelMode : std::uint8_t
  {
    Default,
    Elapsed,
    Duration
  };

  /**
   * @brief A composite widget providing a formatted time display (current / total).
   */
  class TimeLabel final
  {
  public:
    explicit TimeLabel(rt::PlaybackService& playbackService, TimeLabelMode mode = TimeLabelMode::Default);

    Gtk::Widget& widget() { return _label; }

  private:
    void reset();
    void updateLabel(std::uint32_t posMs, std::uint32_t durMs);

    rt::PlaybackService& _playbackService;
    TimeLabelMode _mode;
    Gtk::Label _label;
    PlaybackPositionInterpolator _interpolator;

    rt::Subscription _startedSub;
    rt::Subscription _pausedSub;
    rt::Subscription _idleSub;
    rt::Subscription _stoppedSub;
    rt::Subscription _preparingSub;
    rt::Subscription _seekUpdateSub;

    bool _isPreviewing = false;
    std::uint32_t _lastPosSec = 0;
    std::uint32_t _lastDurSec = 0;
    bool _dirty = true;
  };
} // namespace ao::gtk
