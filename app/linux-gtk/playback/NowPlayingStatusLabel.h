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
    explicit NowPlayingStatusLabel(rt::PlaybackService& playbackService);
    ~NowPlayingStatusLabel();

    // Not copyable or movable
    NowPlayingStatusLabel(NowPlayingStatusLabel const&) = delete;
    NowPlayingStatusLabel& operator=(NowPlayingStatusLabel const&) = delete;
    NowPlayingStatusLabel(NowPlayingStatusLabel&&) = delete;
    NowPlayingStatusLabel& operator=(NowPlayingStatusLabel&&) = delete;

    Gtk::Widget& widget() { return _label; }

  private:
    void updateState();

    rt::PlaybackService& _playbackService;
    Gtk::Label _label;

    rt::Subscription _startedSub;
    rt::Subscription _pausedSub;
    rt::Subscription _idleSub;
    rt::Subscription _stoppedSub;
  };
} // namespace ao::gtk
