// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/PlaybackService.h>
#include <ao/uimodel/playback/PlaybackPositionInterpolator.h>
#include <ao/uimodel/playback/PlaybackTimeViewModel.h>

#include <gtkmm/label.h>
#include <gtkmm/widget.h>

#include <cstdint>
#include <memory>

namespace ao::gtk
{
  /**
   * @brief A composite widget providing a formatted time display (current / total).
   */
  class TimeLabel final
  {
  public:
    using Mode = uimodel::playback::PlaybackTimeMode;

    explicit TimeLabel(rt::PlaybackService& playbackService, Mode mode = Mode::Default);
    ~TimeLabel();

    TimeLabel(TimeLabel const&) = delete;
    TimeLabel& operator=(TimeLabel const&) = delete;
    TimeLabel(TimeLabel&&) = delete;
    TimeLabel& operator=(TimeLabel&&) = delete;

    Gtk::Widget& widget() { return _label; }

  private:
    void applyState(uimodel::playback::PlaybackTimeViewState const& view);
    void reset();
    void updateLabel(std::uint32_t posMs, std::uint32_t durMs);

    Mode _mode;
    Gtk::Label _label;
    uimodel::playback::PlaybackPositionInterpolator _interpolator;
    std::unique_ptr<uimodel::playback::PlaybackTimeViewModel> _controllerPtr;

    bool _isPreviewing = false;
    std::uint32_t _lastPosSec = 0;
    std::uint32_t _lastDurSec = 0;
    bool _dirty = true;
  };
} // namespace ao::gtk
