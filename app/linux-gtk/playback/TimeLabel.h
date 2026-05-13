// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "playback/PlaybackPositionInterpolator.h"
#include "runtime/PlaybackService.h"
#include <gtkmm/label.h>

namespace ao::gtk
{
  /**
   * @brief A composite widget providing a formatted time display (current / total).
   */
  class TimeLabel final
  {
  public:
    explicit TimeLabel(rt::PlaybackService& playbackService);

    Gtk::Widget& widget() { return _label; }

  private:
    void reset();
    void updateLabel(std::uint32_t posMs, std::uint32_t durMs);

    rt::PlaybackService& _playbackService;
    Gtk::Label _label;
    PlaybackPositionInterpolator _interpolator;

    rt::Subscription _startedSub;
    rt::Subscription _pausedSub;
    rt::Subscription _idleSub;
    rt::Subscription _stoppedSub;
    rt::Subscription _preparingSub;
  };
} // namespace ao::gtk
