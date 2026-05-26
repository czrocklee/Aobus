// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/playback/NowPlayingViewModel.h>

#include <gtkmm/label.h>
#include <gtkmm/widget.h>

#include <memory>

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
    void applyState(uimodel::playback::NowPlayingViewState const& view);

    rt::PlaybackService& _playbackService;
    Gtk::Label _label;

    std::unique_ptr<uimodel::playback::NowPlayingViewModel> _controller;
  };
} // namespace ao::gtk
