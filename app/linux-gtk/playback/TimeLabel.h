// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/playback/seek/PlaybackPositionInterpolator.h>
#include <ao/uimodel/playback/seek/PlaybackTimeViewModel.h>

#include <gtkmm/label.h>
#include <gtkmm/widget.h>

#include <chrono>

namespace ao::rt
{
  class PlaybackService;
}

namespace ao::gtk
{
  class TimeLabel final
  {
  public:
    using Mode = uimodel::PlaybackTimeMode;

    TimeLabel(rt::PlaybackService& playbackService, Mode mode);

    TimeLabel(TimeLabel const&) = delete;
    TimeLabel& operator=(TimeLabel const&) = delete;
    TimeLabel(TimeLabel&&) = delete;
    TimeLabel& operator=(TimeLabel&&) = delete;

    ~TimeLabel();

    Gtk::Widget& widget() { return _label; }

  private:
    void applyState(uimodel::PlaybackTimeViewState const& view);
    void reset();

    void updateLabel(std::chrono::milliseconds elapsed, std::chrono::milliseconds duration);

    Mode _mode;
    Gtk::Label _label;
    uimodel::PlaybackTimeViewModel _controller;
    uimodel::PlaybackPositionInterpolator _interpolator;

    bool _isPreviewing = false;
    bool _dirty = true;
    std::chrono::seconds _lastElapsed{0};
    std::chrono::seconds _lastDuration{0};
  };
} // namespace ao::gtk
