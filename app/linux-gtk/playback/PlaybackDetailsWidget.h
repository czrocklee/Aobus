// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/playback/NowPlayingViewModel.h>

#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/widget.h>

namespace ao::rt
{
  class PlaybackService;
}

namespace ao::gtk
{
  /**
   * PlaybackDetailsWidget displays source stream information (sample rate, bit depth)
   * and an audio quality status icon with detailed pipeline tooltips.
   */
  class PlaybackDetailsWidget final
  {
  public:
    explicit PlaybackDetailsWidget(rt::PlaybackService& playbackService);
    ~PlaybackDetailsWidget();

    // Not copyable or movable
    PlaybackDetailsWidget(PlaybackDetailsWidget const&) = delete;
    PlaybackDetailsWidget& operator=(PlaybackDetailsWidget const&) = delete;
    PlaybackDetailsWidget(PlaybackDetailsWidget&&) = delete;
    PlaybackDetailsWidget& operator=(PlaybackDetailsWidget&&) = delete;

    Gtk::Widget& widget() { return _container; }

  private:
    friend class PlaybackDetailsWidgetTestPeer;

    void applyState(uimodel::playback::NowPlayingViewState const& view);

    Gtk::Box _container{Gtk::Orientation::HORIZONTAL};
    Gtk::Label _streamInfoLabel;
    Gtk::Image _sinkStatusIcon;

    uimodel::playback::NowPlayingViewModel _controller;
  };
} // namespace ao::gtk
