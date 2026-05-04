// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/Player.h>
#include <ao/audio/Types.h>

#include <giomm.h>
#include <gtkmm.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "AobusSoul.h"
#include "AobusSoulWindow.h"
#include "VolumeBar.h"

namespace ao::gtk
{
  class PlaybackBar final : public Gtk::Box
  {
  public:
    using PlaySignal = sigc::signal<void()>;
    using PauseSignal = sigc::signal<void()>;
    using StopSignal = sigc::signal<void()>;
    using SeekSignal = sigc::signal<void(std::uint32_t)>;
    using OutputChangedSignal = sigc::signal<void(ao::audio::BackendId, ao::audio::DeviceId, ao::audio::ProfileId)>;
    using VolumeChangedSignal = sigc::signal<void(float)>;
    using MuteToggledSignal = sigc::signal<void()>;

    PlaybackBar();
    ~PlaybackBar() override;

    void setSnapshot(ao::audio::Player::Status const& status);
    void setInteractive(bool enabled);

    PlaySignal& signalPlayRequested();
    PauseSignal& signalPauseRequested();
    StopSignal& signalStopRequested();
    SeekSignal& signalSeekRequested();
    OutputChangedSignal& signalOutputChanged();
    VolumeChangedSignal& signalVolumeChanged();
    MuteToggledSignal& signalMuteToggled();

  private:
    struct LastState final
    {
      ao::audio::Engine::Status engine;
      std::uint32_t positionSec = 0xFFFFFFFF;
      std::uint32_t durationSec = 0xFFFFFFFF;
      std::vector<ao::audio::IBackendProvider::Status> availableBackends;
      ao::audio::Quality quality = ao::audio::Quality::Unknown;
      bool isReady = false;
    };

    // Layout constants
    static constexpr int kWidthChars = 7;
    static constexpr int kOutputScrolledMinHeight = 320;
    static constexpr int kOutputScrolledMinWidth = 360;
    static constexpr double kLogoAspectRatio = 1.0;
    static constexpr int kOutputIconVerticalInset = 6;
    static constexpr int kOutputIconMinHeight = 22;

    void setupLayout();
    void setupSignals();
    void updateTransportButtons(ao::audio::Transport state);

    Gtk::Widget* createOutputWidget(Glib::RefPtr<Glib::Object> const& item);
    void updateOutputModel(ao::audio::Player::Status const& status);
    void updateOutputLabel(ao::audio::Player::Status const& status);
    void updateOutputIcon(ao::audio::Quality quality);
    void syncOutputIconSize();
    void triggerSoulEasterEgg();

    // Output selection
    Gtk::Button _outputButton;
    AobusSoul _outputSoul;
    std::unique_ptr<AobusSoulWindow> _soulWindow;

    Gtk::Popover _outputPopover;
    Gtk::ListBox _outputListBox;
    Glib::RefPtr<Gio::ListStore<Glib::Object>> _outputStore;

    // Transport controls
    Gtk::Button _playButton;
    Gtk::Button _pauseButton;
    Gtk::Button _stopButton;

    // Seek and time
    Gtk::Scale _seekScale;
    Gtk::Label _timeLabel;

    // Volume and mute
    Gtk::ToggleButton _muteButton;
    VolumeBar _volumeScale;

    PlaySignal _playRequested;
    PauseSignal _pauseRequested;
    StopSignal _stopRequested;
    SeekSignal _seekRequested;
    OutputChangedSignal _outputChanged;
    VolumeChangedSignal _volumeChanged;
    MuteToggledSignal _muteToggled;

    bool _updatingSeekScale = false;
    bool _updatingVolumeScale = false;

    ao::audio::Quality _lastIconQuality = ao::audio::Quality::Unknown;
    sigc::connection _soulLongPressTimer;
    std::uint32_t _tickCallbackId = 0;
    std::int64_t _firstFrameTime = 0;
    double _animationTimeSec = 0.0;

    int _outputIconWidth = 0;
    int _outputIconHeight = 0;

    LastState _lastState;
  };
} // namespace ao::gtk
