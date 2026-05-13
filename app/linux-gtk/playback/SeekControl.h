// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "playback/PlaybackPositionInterpolator.h"
#include "runtime/PlaybackService.h"
#include <gtkmm/scale.h>

namespace ao::gtk
{
  /**
   * @brief A composite widget providing a seek slider for playback.
   */
  class SeekControl final
  {
  public:
    explicit SeekControl(ao::rt::PlaybackService& playbackService);

    Gtk::Widget& widget() { return _scale; }

  private:
    void reset();

    ao::rt::PlaybackService& _playbackService;
    Gtk::Scale _scale;
    PlaybackPositionInterpolator _interpolator;
    bool _isDragging = false;
    bool _updating = false;

    ao::rt::Subscription _startedSub;
    ao::rt::Subscription _pausedSub;
    ao::rt::Subscription _idleSub;
    ao::rt::Subscription _stoppedSub;
    ao::rt::Subscription _preparingSub;
  };
} // namespace ao::gtk
