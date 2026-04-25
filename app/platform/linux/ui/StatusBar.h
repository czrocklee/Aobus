// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/playback/PlaybackTypes.h"

#include <gtkmm.h>

#include <chrono>
#include <string>

namespace app::ui
{

  class StatusBar : public Gtk::Box
  {
  public:
    StatusBar();
    ~StatusBar() override;

    void showMessage(std::string const& message, std::chrono::seconds duration = std::chrono::seconds{5});
    void clearMessage();

    void setTrackCount(std::size_t count);
    void setSelectionInfo(std::size_t count, std::optional<std::chrono::milliseconds> totalDuration = std::nullopt);
    void setPlaybackDetails(app::core::playback::PlaybackSnapshot const& snapshot);

    void setImportProgress(double fraction, std::string const& info);
    void clearImportProgress();

    using NowPlayingClickedSignal = sigc::signal<void()>;
    NowPlayingClickedSignal& signalNowPlayingClicked() { return _nowPlayingClicked; }

    using BackendChangedSignal = sigc::signal<void(app::core::playback::BackendKind)>;
    BackendChangedSignal& signalBackendChanged() { return _backendChanged; }

  private:
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
    Gtk::MenuButton _backendButton;
    Gtk::Label _streamInfoLabel;
    Gtk::Image _sinkStatusIcon;

    // Far Right: Status message
    Gtk::Label _statusLabel;

    sigc::connection _timerConnection;
    NowPlayingClickedSignal _nowPlayingClicked;
    BackendChangedSignal _backendChanged;
  };

} // namespace app::ui
