// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/audio/Types.h>

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

    void setSnapshot(rs::audio::Snapshot const& snapshot);
    void setInteractive(bool enabled);

    PlaySignal& signalPlayRequested();
    PauseSignal& signalPauseRequested();
    StopSignal& signalStopRequested();
    SeekSignal& signalSeekRequested();

  private:
    void setupLayout();
    void setupSignals();
    void updateTransportButtons(rs::audio::Transport state);

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

    struct LastState final
    {
      rs::audio::Transport transport = rs::audio::Transport::Idle;
      std::uint32_t positionSec = 0xFFFFFFFF;
      std::uint32_t durationSec = 0xFFFFFFFF;
    } _lastState;

    // Layout constants
    static constexpr int kWidthChars = 7;
  };
} // namespace app::ui
