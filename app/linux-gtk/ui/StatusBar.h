// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/Engine.h>
#include <ao/audio/Player.h>
#include <ao/audio/Types.h>
#include <runtime/CorePrimitives.h>

#include <gtkmm.h>

#include <chrono>
#include <string>

namespace ao::rt
{
  class AppSession;
  struct PlaybackState;
}

namespace ao::gtk
{
  class StatusBar final : public Gtk::Box
  {
  public:
    explicit StatusBar(ao::rt::AppSession& session);
    ~StatusBar() override;

    void showMessage(std::string_view message, std::chrono::seconds duration = std::chrono::seconds{5});
    void clearMessage();

    void setTrackCount(std::size_t count);
    void setSelectionInfo(std::size_t count, std::optional<std::chrono::milliseconds> totalDuration = std::nullopt);
    void setPlaybackDetails(ao::audio::Player::Status const& status);
    void setPlaybackState(struct ao::rt::PlaybackState const& state);

    void setImportProgress(double fraction, std::string_view info);
    void clearImportProgress();

  private:
    struct LastPlaybackState final
    {
      ao::audio::Engine::Status engine;
      std::string title;
      std::string artist;
      std::uint32_t underrunCount = 0;
      ao::audio::Quality quality = ao::audio::Quality::Unknown;
      std::string qualityTooltip;
      ao::audio::flow::Graph flow;
    };

    // Layout constants
    static constexpr int kImportProgressWidth = 200;
    static constexpr int kTransitionDurationMs = 250;

    void applyInitialState();
    void updatePlaybackStatusLabels(ao::audio::Player::Status const& status);
    void updatePlaybackTooltip(ao::audio::Player::Status const& status);

    // Left: Library info
    Gtk::Label _libraryLabel;

    // Middle-Left: Selection info
    Gtk::Label _selectionLabel;

    // Center: Now playing info
    Gtk::Label _nowPlayingLabel;

    // Middle-Right: Import progress (hidden by default)
    Gtk::Box _importBox;
    Gtk::ProgressBar _importProgressBar;
    Gtk::Label _importLabel;

    // Right: Playback details
    Gtk::Box _playbackDetailsBox{Gtk::Orientation::HORIZONTAL};
    Gtk::Label _streamInfoLabel;
    Gtk::Image _sinkStatusIcon;

    // Far Right: Status message
    Gtk::Label _statusLabel;

    sigc::connection _timerConnection;
    ao::rt::AppSession& _session;
    ao::rt::Subscription _startedSub;
    ao::rt::Subscription _pausedSub;
    ao::rt::Subscription _idleSub;
    ao::rt::Subscription _stoppedSub;
    ao::rt::Subscription _outputChangedSub;
    ao::rt::Subscription _qualityChangedSub;
    ao::rt::Subscription _notificationPostedSub;
    ao::rt::Subscription _selectionChangedSub;
    ao::rt::Subscription _importCompletedSub;

    LastPlaybackState _lastPlaybackState;
    std::string _lastTooltipText;
  };
} // namespace ao::gtk
