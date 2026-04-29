// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "platform/linux/ui/PlaybackBar.h"
#include "platform/linux/ui/LayoutConstants.h"

#include <gtkmm/button.h>
#include <gtkmm/scale.h>

namespace app::ui
{

  PlaybackBar::PlaybackBar()
    : Gtk::Box(Gtk::Orientation::HORIZONTAL)
  {
    setupLayout();
    setupSignals();
  }

  PlaybackBar::~PlaybackBar() = default;

  void PlaybackBar::setupLayout()
  {
    set_spacing(Layout::kMarginMedium);
    set_margin(Layout::kMarginSmall);

    // Transport controls box
    auto* transportBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    transportBox->set_spacing(0); // 0 because we use 'linked' class
    transportBox->set_halign(Gtk::Align::CENTER);
    transportBox->set_valign(Gtk::Align::CENTER);
    transportBox->add_css_class("linked");

    _playButton.set_icon_name("media-playback-start-symbolic");
    _playButton.set_tooltip_text("Play");
    _playButton.set_sensitive(false);

    _pauseButton.set_icon_name("media-playback-pause-symbolic");
    _pauseButton.set_tooltip_text("Pause");
    _pauseButton.set_sensitive(false);
    _pauseButton.set_visible(false);

    _stopButton.set_icon_name("media-playback-stop-symbolic");
    _stopButton.set_tooltip_text("Stop");
    _stopButton.set_sensitive(false);

    transportBox->append(_playButton);
    transportBox->append(_pauseButton);
    transportBox->append(_stopButton);

    // Seek and time box
    auto* seekBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    seekBox->set_spacing(Layout::kSpacingXLarge);
    seekBox->set_hexpand(true);
    seekBox->set_halign(Gtk::Align::FILL);
    seekBox->set_valign(Gtk::Align::CENTER);

    _seekScale.set_range(0, 100);
    _seekScale.set_value(0);
    _seekScale.set_sensitive(false);
    _seekScale.set_hexpand(true);
    _seekScale.set_valign(Gtk::Align::CENTER);

    _timeLabel.set_text("0:00");
    _timeLabel.set_halign(Gtk::Align::END);
    _timeLabel.set_valign(Gtk::Align::CENTER);
    _timeLabel.set_width_chars(kWidthChars);

    seekBox->append(_seekScale);
    seekBox->append(_timeLabel);

    // Add all to main horizontal box
    append(*transportBox);
    append(*seekBox);
  }

  void PlaybackBar::setupSignals()
  {
    _playButton.signal_clicked().connect([this]() { _playRequested.emit(); });

    _pauseButton.signal_clicked().connect([this]() { _pauseRequested.emit(); });

    _stopButton.signal_clicked().connect([this]() { _stopRequested.emit(); });

    _seekScale.signal_value_changed().connect(
      [this]()
      {
        if (_updatingSeekScale)
        {
          return;
        }

        auto position = static_cast<std::uint32_t>(_seekScale.get_value());
        _seekRequested.emit(position);
      });
  }

  void PlaybackBar::setSnapshot(app::core::playback::PlaybackSnapshot const& snapshot)
  {
    auto const posSec = snapshot.positionMs / 1000;
    auto const durSec = snapshot.durationMs / 1000;

    if (snapshot.state != _lastState.state || posSec != _lastState.positionSec || durSec != _lastState.durationSec)
    {
      _lastState = {.state = snapshot.state, .positionSec = posSec, .durationSec = durSec};

      updateTransportButtons(snapshot.state);

      if (snapshot.state == app::core::playback::TransportState::Idle)
      {
        _timeLabel.set_text("00:00 / 00:00");
      }
      else
      {
        auto durationMin = durSec / 60;
        auto durationRemSec = durSec % 60;
        auto positionMin = posSec / 60;
        auto positionRemSec = posSec % 60;
        _timeLabel.set_text(
          std::format("{:d}:{:02d} / {:d}:{:02d}", positionMin, positionRemSec, durationMin, durationRemSec));
      }
    }

    // Always update seek scale sensitivity and range for smoothness
    if (snapshot.state == app::core::playback::TransportState::Idle)
    {
      _seekScale.set_range(0, 100);
      _seekScale.set_value(0);
      _seekScale.set_sensitive(false);
      return;
    }

    // Update seek scale
    _updatingSeekScale = true;

    if (snapshot.durationMs > 0)
    {
      _seekScale.set_range(0, static_cast<double>(snapshot.durationMs));
      _seekScale.set_value(static_cast<double>(snapshot.positionMs));
      _seekScale.set_sensitive(true);
    }
    else
    {
      _seekScale.set_range(0, 100);
      _seekScale.set_value(0);
      _seekScale.set_sensitive(false);
    }

    _updatingSeekScale = false;
  }

  void PlaybackBar::setInteractive(bool enabled)
  {
    _playButton.set_sensitive(enabled);
    _stopButton.set_sensitive(enabled);
    _seekScale.set_sensitive(enabled);
  }

  void PlaybackBar::updateTransportButtons(app::core::playback::TransportState state)
  {
    switch (state)
    {
      case app::core::playback::TransportState::Idle:
      case app::core::playback::TransportState::Stopping:
      case app::core::playback::TransportState::Error:
        _playButton.set_visible(true);
        _pauseButton.set_visible(false);
        _playButton.set_sensitive(state != app::core::playback::TransportState::Idle);
        _pauseButton.set_sensitive(false);
        _stopButton.set_sensitive(false);
        _seekScale.set_sensitive(false);
        break;

      case app::core::playback::TransportState::Opening:
      case app::core::playback::TransportState::Buffering:
        _playButton.set_visible(true);
        _pauseButton.set_visible(false);
        _playButton.set_sensitive(false);
        _pauseButton.set_sensitive(false);
        _stopButton.set_sensitive(true);
        _seekScale.set_sensitive(false);
        break;

      case app::core::playback::TransportState::Playing:
        _playButton.set_visible(false);
        _pauseButton.set_visible(true);
        _playButton.set_sensitive(false);
        _pauseButton.set_sensitive(true);
        _stopButton.set_sensitive(true);
        _seekScale.set_sensitive(true);
        break;

      case app::core::playback::TransportState::Paused:
        _playButton.set_visible(true);
        _pauseButton.set_visible(false);
        _playButton.set_sensitive(true);
        _pauseButton.set_sensitive(false);
        _stopButton.set_sensitive(true);
        _seekScale.set_sensitive(true);
        break;

      case app::core::playback::TransportState::Seeking:
        _playButton.set_visible(true);
        _pauseButton.set_visible(false);
        _playButton.set_sensitive(false);
        _pauseButton.set_sensitive(false);
        _stopButton.set_sensitive(true);
        _seekScale.set_sensitive(false);
        break;
    }
  }

  PlaybackBar::PlaySignal& PlaybackBar::signalPlayRequested()
  {
    return _playRequested;
  }

  PlaybackBar::PauseSignal& PlaybackBar::signalPauseRequested()
  {
    return _pauseRequested;
  }

  PlaybackBar::StopSignal& PlaybackBar::signalStopRequested()
  {
    return _stopRequested;
  }

  PlaybackBar::SeekSignal& PlaybackBar::signalSeekRequested()
  {
    return _seekRequested;
  }

} // namespace app::ui
