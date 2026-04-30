// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/audio/PlaybackTypes.h>

#include <gtkmm.h>

#include <chrono>
#include <string>

namespace app::ui
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
    void setPlaybackDetails(rs::audio::PlaybackSnapshot const& snapshot);

    void setImportProgress(double fraction, std::string const& info);
    void clearImportProgress();

    using NowPlayingClickedSignal = sigc::signal<void()>;
    NowPlayingClickedSignal& signalNowPlayingClicked() { return _nowPlayingClicked; }

    using OutputChangedSignal = sigc::signal<void(rs::audio::BackendKind, std::string)>;
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

    struct LastPlaybackState final
    {
      rs::audio::TransportState state = rs::audio::TransportState::Idle;
      rs::audio::BackendKind backend = rs::audio::BackendKind::None;
      std::string title;
      std::string artist;
      std::uint32_t underrunCount = 0;
      rs::audio::AudioQuality quality = rs::audio::AudioQuality::Unknown;
      std::string qualityTooltip;
      rs::audio::AudioGraph graph;
      std::string currentDeviceId;
      std::vector<rs::audio::BackendSnapshot> availableBackends;
    } _lastPlaybackState;
    std::string _lastTooltipText;

    // Layout constants
    static constexpr int kOutputScrolledMinHeight = 320;
    static constexpr int kOutputScrolledMinWidth = 360;
    static constexpr int kImportProgressWidth = 200;
    static constexpr int kTransitionDurationMs = 250;
  };

} // namespace app::ui
