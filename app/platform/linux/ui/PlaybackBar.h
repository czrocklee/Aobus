// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/playback/PlaybackTypes.h"

#include <gtkmm.h>

#include <cstdint>

namespace app::ui
{

  class PlaybackBar final : public Gtk::Box
  {
  public:
    using PlaySignal = sigc::signal<void()>;
    using PauseSignal = sigc::signal<void()>;
    using StopSignal = sigc::signal<void()>;
    using SeekSignal = sigc::signal<void(std::uint32_t)>;

    PlaybackBar();
    ~PlaybackBar() override;

    void setSnapshot(app::core::playback::PlaybackSnapshot const& snapshot);
    void setInteractive(bool enabled);

    PlaySignal& signalPlayRequested();
    PauseSignal& signalPauseRequested();
    StopSignal& signalStopRequested();
    SeekSignal& signalSeekRequested();

  private:
    void setupLayout();
    void setupSignals();
    void updateTransportButtons(app::core::playback::TransportState state);

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
    bool _updatingSeekScale = false;

    struct LastState
    {
      app::core::playback::TransportState state = app::core::playback::TransportState::Idle;
      std::uint32_t positionSec = 0xFFFFFFFF;
      std::uint32_t durationSec = 0xFFFFFFFF;
    } _lastState;
  };

} // namespace app::ui
