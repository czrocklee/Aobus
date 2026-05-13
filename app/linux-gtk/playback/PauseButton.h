// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "runtime/PlaybackService.h"
#include <gtkmm/button.h>
#include <string>

namespace ao::gtk::playback
{
  /**
   * @brief A composite widget providing a Pause button.
   */
  class PauseButton final
  {
  public:
    PauseButton(ao::rt::PlaybackService& playbackService, bool showLabel = false, std::string const& size = "normal");

    Gtk::Widget& widget() { return _button; }

  private:
    void refresh();

    ao::rt::PlaybackService& _playbackService;
    Gtk::Button _button;

    ao::rt::Subscription _startedSub;
    ao::rt::Subscription _pausedSub;
    ao::rt::Subscription _idleSub;
    ao::rt::Subscription _stoppedSub;
  };
} // namespace ao::gtk::playback
