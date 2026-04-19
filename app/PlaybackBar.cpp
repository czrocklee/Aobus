// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "PlaybackBar.h"

#include <gtkmm/button.h>
#include <gtkmm/scale.h>

PlaybackBar::PlaybackBar()
  : Gtk::Box(Gtk::Orientation::HORIZONTAL)
{
  setupLayout();
  setupSignals();
}

PlaybackBar::~PlaybackBar() = default;

void PlaybackBar::setupLayout()
{
  set_spacing(8);
  set_margin_start(8);
  set_margin_end(8);
  set_margin_top(4);
  set_margin_bottom(4);

  // Transport controls box
  auto* transportBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  transportBox->set_spacing(4);
  transportBox->set_halign(Gtk::Align::CENTER);
  transportBox->set_valign(Gtk::Align::CENTER);

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
  seekBox->set_spacing(6);
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
  _timeLabel.set_width_chars(7);

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

void PlaybackBar::setSnapshot(app::playback::PlaybackSnapshot const& snapshot)
{
  // Update time
  auto durationSec = snapshot.durationMs / 1000;
  auto positionSec = snapshot.positionMs / 1000;
  auto durationMin = durationSec / 60;
  auto durationRemSec = durationSec % 60;
  auto positionMin = positionSec / 60;
  auto positionRemSec = positionSec % 60;
  _timeLabel.set_text(
    std::format("{:d}:{:02d} / {:d}:{:02d}", positionMin, positionRemSec, durationMin, durationRemSec));

  // Update seek scale
  _updatingSeekScale = true;
  if (snapshot.durationMs > 0)
  {
    _seekScale.set_range(0, static_cast<double>(snapshot.durationMs));
    _seekScale.set_value(static_cast<double>(snapshot.positionMs));
  }
  else
  {
    _seekScale.set_range(0, 100);
    _seekScale.set_value(0);
  }
  _updatingSeekScale = false;

  // Update transport buttons
  updateTransportButtons(snapshot.state);
}

void PlaybackBar::setInteractive(bool enabled)
{
  _playButton.set_sensitive(enabled);
  _stopButton.set_sensitive(enabled);
  _seekScale.set_sensitive(enabled);
}

void PlaybackBar::updateTransportButtons(app::playback::TransportState state)
{
  switch (state)
  {
    case app::playback::TransportState::Idle:
    case app::playback::TransportState::Stopping:
    case app::playback::TransportState::Error:
      _playButton.set_visible(true);
      _pauseButton.set_visible(false);
      _playButton.set_sensitive(state != app::playback::TransportState::Idle);
      _pauseButton.set_sensitive(false);
      _stopButton.set_sensitive(false);
      _seekScale.set_sensitive(false);
      break;

    case app::playback::TransportState::Opening:
    case app::playback::TransportState::Buffering:
      _playButton.set_visible(true);
      _pauseButton.set_visible(false);
      _playButton.set_sensitive(false);
      _pauseButton.set_sensitive(false);
      _stopButton.set_sensitive(true);
      _seekScale.set_sensitive(false);
      break;

    case app::playback::TransportState::Playing:
      _playButton.set_visible(false);
      _pauseButton.set_visible(true);
      _playButton.set_sensitive(false);
      _pauseButton.set_sensitive(true);
      _stopButton.set_sensitive(true);
      _seekScale.set_sensitive(true);
      break;

    case app::playback::TransportState::Paused:
      _playButton.set_visible(true);
      _pauseButton.set_visible(false);
      _playButton.set_sensitive(true);
      _pauseButton.set_sensitive(false);
      _stopButton.set_sensitive(true);
      _seekScale.set_sensitive(true);
      break;

    case app::playback::TransportState::Seeking:
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
