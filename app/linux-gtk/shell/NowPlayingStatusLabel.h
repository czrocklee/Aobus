// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <runtime/CorePrimitives.h>

#include <gtkmm/label.h>

#include <string>

namespace ao::rt
{
  class PlaybackService;
}

namespace ao::gtk
{
  /**
   * NowPlayingStatusLabel displays the currently playing track metadata
   * (Artist - Title) and reveals the playing track when clicked.
   */
  class NowPlayingStatusLabel final
  {
  public:
    explicit NowPlayingStatusLabel(ao::rt::PlaybackService& playbackService);
    ~NowPlayingStatusLabel();

    Gtk::Widget& widget() { return _label; }

  private:
    void updateState();

    ao::rt::PlaybackService& _playbackService;
    Gtk::Label _label;

    ao::rt::Subscription _startedSub;
    ao::rt::Subscription _pausedSub;
    ao::rt::Subscription _idleSub;
    ao::rt::Subscription _stoppedSub;
  };
} // namespace ao::gtk
