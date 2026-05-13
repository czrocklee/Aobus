// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/Player.h>
#include <ao/audio/Types.h>
#include <runtime/CorePrimitives.h>

#include <gtkmm/box.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>

#include <string>

namespace ao::rt
{
  class PlaybackService;
  struct PlaybackState;
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

    Gtk::Widget& widget() { return _container; }

  private:
    void updateState();
    void updateTooltip(rt::PlaybackState const& state);

    rt::PlaybackService& _playbackService;
    Gtk::Box _container{Gtk::Orientation::HORIZONTAL};
    Gtk::Label _streamInfoLabel;
    Gtk::Image _sinkStatusIcon;

    rt::Subscription _startedSub;
    rt::Subscription _pausedSub;
    rt::Subscription _idleSub;
    rt::Subscription _stoppedSub;
    rt::Subscription _outputChangedSub;
    rt::Subscription _qualityChangedSub;

    std::string _lastTooltipText;
  };
} // namespace ao::gtk
