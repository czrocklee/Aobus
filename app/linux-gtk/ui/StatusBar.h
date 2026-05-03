// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/Engine.h>
#include <ao/audio/Player.h>
#include <ao/audio/Types.h>

#include <gtkmm.h>

#include <chrono>
#include <string>

namespace ao::gtk
{
  class StatusBar final : public Gtk::Box
  {
  public:
    StatusBar();
    ~StatusBar() override;

    void showMessage(std::string const& message, std::chrono::seconds duration = std::chrono::seconds{5});
    void clearMessage();

    void setTrackCount(std::size_t count);
    void setSelectionInfo(std::size_t count, std::optional<std::chrono::milliseconds> totalDuration = std::nullopt);
    void setPlaybackDetails(ao::audio::Player::Status const& status);

    void setImportProgress(double fraction, std::string const& info);
    void clearImportProgress();

    using NowPlayingClickedSignal = sigc::signal<void()>;
    NowPlayingClickedSignal& signalNowPlayingClicked() { return _nowPlayingClicked; }

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
    NowPlayingClickedSignal _nowPlayingClicked;

    LastPlaybackState _lastPlaybackState;
    std::string _lastTooltipText;
  };
} // namespace ao::gtk
