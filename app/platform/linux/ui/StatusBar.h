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

    using OutputChangedSignal = sigc::signal<void(app::core::backend::BackendKind, std::string)>;
    OutputChangedSignal& signalOutputChanged() { return _outputChanged; }

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
    Gtk::ToggleButton _outputButton;
    Gtk::Popover _outputPopover;
    Gtk::ListBox _outputListBox;
    Glib::RefPtr<Gio::ListStore<Glib::Object>> _outputStore;
    Gtk::Label _streamInfoLabel;
    Gtk::Image _sinkStatusIcon;

    Gtk::Widget* createOutputWidget(Glib::RefPtr<Glib::Object> const& item);

    // Far Right: Status message
    Gtk::Label _statusLabel;

    sigc::connection _timerConnection;
    NowPlayingClickedSignal _nowPlayingClicked;
    OutputChangedSignal _outputChanged;

    struct LastPlaybackState
    {
      app::core::playback::TransportState state = app::core::playback::TransportState::Idle;
      app::core::backend::BackendKind backend = app::core::backend::BackendKind::None;
      std::string title;
      std::string artist;
      std::uint32_t underrunCount = 0;
      app::core::backend::AudioQuality quality = app::core::backend::AudioQuality::Unknown;
      app::core::backend::AudioGraph graph;
      std::string currentDeviceId;
      std::vector<app::core::playback::BackendSnapshot> availableBackends;
    } _lastPlaybackState;
  };

} // namespace app::ui
