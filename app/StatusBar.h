// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <gtkmm.h>
#include "playback/PlaybackTypes.h"

#include <chrono>
#include <string>

class StatusBar : public Gtk::Box
{
public:
  StatusBar();
  ~StatusBar() override;

  void showMessage(std::string const& message, std::chrono::seconds duration = std::chrono::seconds{5});
  void clearMessage();

  void setTrackCount(std::size_t count);
  void setSelectionInfo(std::size_t count, std::optional<std::chrono::milliseconds> totalDuration = std::nullopt);
  void setPlaybackDetails(app::playback::PlaybackSnapshot const& snapshot);
  
  void setImportProgress(double fraction, std::string const& info);
  void clearImportProgress();

private:
  // Left: Library info
  Gtk::Label _libraryLabel;
  
  // Middle-Left: Selection info
  Gtk::Label _selectionLabel;

  // Middle-Right: Import progress (hidden by default)
  Gtk::Box _importBox;
  Gtk::ProgressBar _importProgressBar;
  Gtk::Label _importLabel;

  // Right: Playback details
  Gtk::Label _playbackLabel;

  // Far Right: Status message
  Gtk::Label _statusLabel;
  
  sigc::connection _timerConnection;

  void updateLayoutVisibility();
};
