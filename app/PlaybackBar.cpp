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

  // Cover art - fixed size square
  _coverArt.set_size_request(40, 40);
  _coverArt.set_from_icon_name("audio-x-generic-symbolic");
  _coverArt.set_halign(Gtk::Align::CENTER);
  _coverArt.set_valign(Gtk::Align::CENTER);

  // Track info box (vertical: title on top, artist below)
  auto* infoBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  infoBox->set_spacing(2);
  infoBox->set_halign(Gtk::Align::START);
  infoBox->set_valign(Gtk::Align::CENTER);

  _titleLabel.set_text("No track selected");
  _titleLabel.set_halign(Gtk::Align::START);
  _titleLabel.set_valign(Gtk::Align::END);
  _titleLabel.set_ellipsize(Pango::EllipsizeMode::END);
  _titleLabel.set_max_width_chars(20);

  _artistLabel.set_text("");
  _artistLabel.set_halign(Gtk::Align::START);
  _artistLabel.set_valign(Gtk::Align::START);
  _artistLabel.set_ellipsize(Pango::EllipsizeMode::END);
  _artistLabel.set_max_width_chars(20);

  infoBox->append(_titleLabel);
  infoBox->append(_artistLabel);

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
  append(_coverArt);
  append(*infoBox);
  append(*transportBox);
  append(*seekBox);
}

void PlaybackBar::setupSignals()
{
  _playButton.signal_clicked().connect([this]() {
    _playRequested.emit();
  });

  _pauseButton.signal_clicked().connect([this]() {
    _pauseRequested.emit();
  });

  _stopButton.signal_clicked().connect([this]() {
    _stopRequested.emit();
  });

  _seekScale.signal_value_changed().connect([this]() {
    auto position = static_cast<std::uint32_t>(_seekScale.get_value());
    _seekRequested.emit(position);
  });
}

void PlaybackBar::setSnapshot(app::playback::PlaybackSnapshot const& snapshot)
{
  // Update track info
  _titleLabel.set_text(snapshot.trackTitle.empty() ? "No track selected" : snapshot.trackTitle);
  _artistLabel.set_text(snapshot.trackArtist);

  // Update time
  auto durationSec = snapshot.durationMs / 1000;
  auto positionSec = snapshot.positionMs / 1000;
  auto durationMin = durationSec / 60;
  auto durationRemSec = durationSec % 60;
  auto positionMin = positionSec / 60;
  auto positionRemSec = positionSec % 60;
  _timeLabel.set_text(std::format("{:d}:{:02d} / {:d}:{:02d}", positionMin, positionRemSec, durationMin, durationRemSec));

  // Update seek scale
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
