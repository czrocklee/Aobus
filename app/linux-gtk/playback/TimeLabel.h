// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "playback/PlaybackPositionInterpolator.h"
#include "runtime/PlaybackService.h"
#include <gtkmm/label.h>

namespace ao::gtk::playback
{
  /**
   * @brief A composite widget providing a formatted time display (current / total).
   */
  class TimeLabel final
  {
  public:
    explicit TimeLabel(ao::rt::PlaybackService& playbackService);

    Gtk::Widget& widget() { return _label; }

  private:
    void reset();
    void updateLabel(std::uint32_t posMs, std::uint32_t durMs);

    ao::rt::PlaybackService& _playbackService;
    Gtk::Label _label;
    PlaybackPositionInterpolator _interpolator;

    ao::rt::Subscription _startedSub;
    ao::rt::Subscription _pausedSub;
    ao::rt::Subscription _idleSub;
    ao::rt::Subscription _stoppedSub;
    ao::rt::Subscription _preparingSub;
  };
} // namespace ao::gtk::playback
