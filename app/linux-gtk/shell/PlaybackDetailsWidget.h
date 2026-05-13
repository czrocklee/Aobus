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
  class AppSession;
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
    explicit PlaybackDetailsWidget(ao::rt::AppSession& session);
    ~PlaybackDetailsWidget();

    Gtk::Widget& widget() { return _container; }

  private:
    void updateState();
    void updateTooltip(ao::rt::PlaybackState const& state);

    ao::rt::AppSession& _session;
    Gtk::Box _container{Gtk::Orientation::HORIZONTAL};
    Gtk::Label _streamInfoLabel;
    Gtk::Image _sinkStatusIcon;

    ao::rt::Subscription _startedSub;
    ao::rt::Subscription _pausedSub;
    ao::rt::Subscription _idleSub;
    ao::rt::Subscription _stoppedSub;
    ao::rt::Subscription _outputChangedSub;
    ao::rt::Subscription _qualityChangedSub;

    std::string _lastTooltipText;
  };
} // namespace ao::gtk
