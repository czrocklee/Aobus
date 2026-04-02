// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "playback/PlaybackTypes.h"

#include <gtkmm.h>

#include <cstdint>

class PlaybackBar final : public Gtk::Box
{
public:
  using PlaySignal = sigc::signal<void()>;
  using PauseSignal = sigc::signal<void()>;
  using StopSignal = sigc::signal<void()>;
  using SeekSignal = sigc::signal<void(std::uint32_t)>;

  PlaybackBar();
  ~PlaybackBar() override;

  void setSnapshot(app::playback::PlaybackSnapshot const& snapshot);
  void setInteractive(bool enabled);

  PlaySignal& signalPlayRequested();
  PauseSignal& signalPauseRequested();
  StopSignal& signalStopRequested();
  SeekSignal& signalSeekRequested();

private:
  void setupLayout();
  void setupSignals();
  void updateTransportButtons(app::playback::TransportState state);

  // Cover art
  Gtk::Image _coverArt;

  // Track info
  Gtk::Label _titleLabel;
  Gtk::Label _artistLabel;

  // Transport controls
  Gtk::Button _playButton;
  Gtk::Button _pauseButton;
  Gtk::Button _stopButton;

  // Seek and time
  Gtk::Scale _seekScale;
  Gtk::Label _timeLabel;

  PlaySignal _playRequested;
  PauseSignal _pauseRequested;
  StopSignal _stopRequested;
  SeekSignal _seekRequested;
};
