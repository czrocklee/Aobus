// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/Player.h>
#include <ao/audio/Types.h>
#include <runtime/StateTypes.h>

namespace ao::app
{
  class AppSession;
}

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
    explicit PlaybackBar(ao::app::AppSession& session);
    ~PlaybackBar() override;

    void applyInitialState();
    void setInteractive(bool enabled);

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
    void updateTransportButtons(ao::audio::Transport state, bool isReady);

    Gtk::Widget* createOutputWidget(Glib::RefPtr<Glib::Object> const& item);
    void updateOutputModel(ao::audio::Player::Status const& status);
    void updateOutputLabel(ao::audio::Player::Status const& status);
    void updateOutputIcon(ao::audio::Quality quality);
    void syncOutputIconSize();
    void triggerSoulEasterEgg();

    void rebuildOutputList();
    void updateOutputTooltip(ao::app::PlaybackState const& state);

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

    ao::app::AppSession& _session;

    ao::app::Subscription _preparingSub;
    ao::app::Subscription _startedSub;
    ao::app::Subscription _pausedSub;
    ao::app::Subscription _idleSub;
    ao::app::Subscription _stoppedSub;
    ao::app::Subscription _outputChangedSub;
    ao::app::Subscription _devicesChangedSub;
    ao::app::Subscription _qualityChangedSub;

    bool _isPlaying = false;
    bool _updatingSeekScale = false;
    bool _isDraggingSeek = false;
    bool _updatingVolumeScale = false;

    ao::audio::Quality _lastIconQuality = ao::audio::Quality::Unknown;
    sigc::connection _soulLongPressTimer;
    std::uint32_t _tickCallbackId = 0;
    std::int64_t _firstFrameTime = 0;
    double _animationTimeSec = 0.0;

    int _outputIconWidth = 0;
    int _outputIconHeight = 0;

    LastState _lastState;

    // Position tracking for display-synchronized updates
    std::uint32_t _lastPositionMs = 0;
    std::uint32_t _lastDurationMs = 0;
    ao::audio::Transport _lastTransport = ao::audio::Transport::Idle;
  };
} // namespace ao::gtk
