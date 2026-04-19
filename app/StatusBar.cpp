// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "StatusBar.h"
#include <iomanip>
#include <sstream>

namespace
{
  std::string formatDuration(std::chrono::milliseconds ms)
  {
    auto totalSeconds = std::chrono::duration_cast<std::chrono::seconds>(ms).count();
    auto minutes = totalSeconds / 60;
    auto seconds = totalSeconds % 60;
    auto hours = minutes / 60;
    minutes %= 60;

    std::stringstream ss;
    if (hours > 0)
    {
      ss << hours << ":" << std::setfill('0') << std::setw(2) << minutes << ":" << std::setw(2) << seconds;
    }
    else
    {
      ss << minutes << ":" << std::setfill('0') << std::setw(2) << seconds;
    }
    return ss.str();
  }
}

StatusBar::StatusBar()
  : Gtk::Box{Gtk::Orientation::HORIZONTAL}
{
  set_hexpand(true);
  set_valign(Gtk::Align::END);
  add_css_class("status-bar");

  // Add a subtle top border via a separator if needed, 
  // but for now we just use margins to increase height.
  
  // Library info (Left)
  _libraryLabel.set_margin_start(12);
  _libraryLabel.set_margin_end(12);
  _libraryLabel.set_margin_top(6);
  _libraryLabel.set_margin_bottom(6);
  _libraryLabel.add_css_class("dim-label");
  append(_libraryLabel);

  // Separator
  auto* sep1 = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL);
  sep1->set_margin_top(4);
  sep1->set_margin_bottom(4);
  append(*sep1);

  // Selection info
  _selectionLabel.set_margin_start(12);
  _selectionLabel.set_margin_end(12);
  _selectionLabel.set_margin_top(6);
  _selectionLabel.set_margin_bottom(6);
  _selectionLabel.add_css_class("dim-label");
  append(_selectionLabel);

  // Import box (Middle, hidden by default)
  _importBox.set_orientation(Gtk::Orientation::HORIZONTAL);
  _importBox.set_spacing(12);
  _importBox.set_margin_start(16);
  _importBox.set_hexpand(true);
  _importBox.set_visible(false);

  _importLabel.set_margin_top(6);
  _importLabel.set_margin_bottom(6);
  _importLabel.add_css_class("dim-label");
  _importBox.append(_importLabel);

  _importProgressBar.set_valign(Gtk::Align::CENTER);
  _importProgressBar.set_size_request(200, -1);
  _importBox.append(_importProgressBar);
  append(_importBox);

  // Spacer to push status to the right
  auto* spacer = Gtk::make_managed<Gtk::Box>();
  spacer->set_hexpand(true);
  append(*spacer);

  // Playback details (Right)
  _playbackLabel.set_margin_start(12);
  _playbackLabel.set_margin_end(12);
  _playbackLabel.set_margin_top(6);
  _playbackLabel.set_margin_bottom(6);
  _playbackLabel.add_css_class("dim-label");
  append(_playbackLabel);

  // Status label (Far Right)
  _statusLabel.set_halign(Gtk::Align::END);
  _statusLabel.set_margin_start(12);
  _statusLabel.set_margin_end(12);
  _statusLabel.set_margin_top(6);
  _statusLabel.set_margin_bottom(6);
  _statusLabel.add_css_class("status-message");
  _statusLabel.set_visible(false);
  append(_statusLabel);

  setTrackCount(0);
}

StatusBar::~StatusBar() = default;

void StatusBar::showMessage(std::string const& message, std::chrono::seconds duration)
{
  if (_timerConnection)
  {
    _timerConnection.disconnect();
  }

  _statusLabel.set_text(message);
  _statusLabel.set_visible(true);

  _timerConnection = Glib::signal_timeout().connect_seconds(
    [this]() {
      clearMessage();
      return false;
    },
    duration.count());
}

void StatusBar::clearMessage()
{
  if (_timerConnection)
  {
    _timerConnection.disconnect();
  }
  _statusLabel.set_text("");
  _statusLabel.set_visible(false);
}

void StatusBar::setTrackCount(std::size_t count)
{
  _libraryLabel.set_text(std::to_string(count) + " tracks");
}

void StatusBar::setSelectionInfo(std::size_t count, std::optional<std::chrono::milliseconds> totalDuration)
{
  if (count == 0)
  {
    _selectionLabel.set_text("");
    return;
  }

  std::string text = std::to_string(count) + (count == 1 ? " item selected" : " items selected");
  if (totalDuration && totalDuration->count() > 0)
  {
    text += " (" + formatDuration(*totalDuration) + ")";
  }
  _selectionLabel.set_text(text);
}

void StatusBar::setPlaybackDetails(app::playback::PlaybackSnapshot const& snapshot)
{
  if (snapshot.state == app::playback::TransportState::Idle || !snapshot.activeFormat)
  {
    _playbackLabel.set_text("");
    return;
  }

  auto const& fmt = *snapshot.activeFormat;
  std::stringstream ss;
  
  switch (snapshot.backend)
  {
    case app::playback::BackendKind::PipeWire: ss << "PipeWire"; break;
    case app::playback::BackendKind::AlsaExclusive: ss << "ALSA"; break;
    default: break;
  }

  if (fmt.sampleRate > 0)
  {
    ss << " | " << (fmt.sampleRate / 1000.0) << " kHz";
    ss << " | " << static_cast<int>(fmt.bitDepth) << " bit";
    if (fmt.channels == 1) ss << " | Mono";
    else if (fmt.channels == 2) ss << " | Stereo";
    else ss << " | " << static_cast<int>(fmt.channels) << " ch";
  }

  if (snapshot.underrunCount > 0)
  {
    ss << " | " << snapshot.underrunCount << " errors";
  }

  _playbackLabel.set_text(ss.str());
}

void StatusBar::setImportProgress(double fraction, std::string const& info)
{
  _importBox.set_visible(true);
  _importProgressBar.set_fraction(fraction);
  _importLabel.set_text(info);
  _selectionLabel.set_visible(false);
}

void StatusBar::clearImportProgress()
{
  _importBox.set_visible(false);
  _selectionLabel.set_visible(true);
}
