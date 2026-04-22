// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "platform/linux/ui/StatusBar.h"

#include <gdkmm/display.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/stylecontext.h>

#include <iomanip>
#include <sstream>

namespace app::ui
{

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

  std::string formatStream(app::core::playback::StreamFormat const& format)
  {
    std::stringstream ss;
    ss << (format.sampleRate / 1000.0) << " kHz/" << static_cast<int>(format.bitDepth) << " bit/";
    if (format.channels == 1)
    {
      ss << "Mono";
    }
    else if (format.channels == 2)
    {
      ss << "Stereo";
    }
    else
    {
      ss << static_cast<int>(format.channels) << " ch";
    }
    return ss.str();
  }

  bool sameStreamFormat(app::core::playback::StreamFormat const& lhs, app::core::playback::StreamFormat const& rhs) noexcept
  {
    return lhs.sampleRate == rhs.sampleRate && lhs.channels == rhs.channels && lhs.bitDepth == rhs.bitDepth &&
           lhs.isFloat == rhs.isFloat && lhs.isInterleaved == rhs.isInterleaved;
  }

  void ensureStatusBarCss()
  {
    static auto provider = []
    {
      auto css = Gtk::CssProvider::create();
      css->load_from_data(R"(
        .sink-status-good { color: #34a853; }
        .sink-status-warning { color: #fbbc04; }
        .sink-status-bad { color: #ea4335; }
      )");
      auto display = Gdk::Display::get_default();
      if (display)
      {
        Gtk::StyleContext::add_provider_for_display(display, css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
      }
      return css;
    }();

    (void)provider;
  }

  void clearSinkStatusClasses(Gtk::Image& image)
  {
    image.remove_css_class("sink-status-good");
    image.remove_css_class("sink-status-warning");
    image.remove_css_class("sink-status-bad");
  }
}

StatusBar::StatusBar()
  : Gtk::Box{Gtk::Orientation::HORIZONTAL}
{
  ensureStatusBarCss();

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
  _playbackDetailsBox.set_spacing(6);
  _playbackDetailsBox.set_margin_start(12);
  _playbackDetailsBox.set_margin_end(12);
  _playbackDetailsBox.set_margin_top(6);
  _playbackDetailsBox.set_margin_bottom(6);
  _playbackLabel.add_css_class("dim-label");
  _sinkLabel.add_css_class("dim-label");
  _sinkLabel.set_visible(false);
  _sinkStatusIcon.set_from_icon_name("media-record-symbolic");
  _sinkStatusIcon.set_pixel_size(12);
  _sinkStatusIcon.set_visible(false);
  _playbackDetailsBox.append(_playbackLabel);
  _playbackDetailsBox.append(_sinkLabel);
  _playbackDetailsBox.append(_sinkStatusIcon);
  append(_playbackDetailsBox);

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

void StatusBar::setPlaybackDetails(app::core::playback::PlaybackSnapshot const& snapshot)
{
  if (snapshot.state == app::core::playback::TransportState::Idle || (!snapshot.sourceFormat && !snapshot.activeFormat))
  {
    _playbackLabel.set_text("");
    _sinkLabel.set_text("");
    _sinkLabel.set_visible(false);
    _sinkLabel.set_tooltip_text("");
    _sinkStatusIcon.set_tooltip_text("");
    _sinkStatusIcon.set_visible(false);
    return;
  }

  std::stringstream ss;
  bool hasText = false;

  switch (snapshot.backend)
  {
    case app::core::playback::BackendKind::PipeWire:
      ss << "PipeWire";
      hasText = true;
      break;
    case app::core::playback::BackendKind::AlsaExclusive:
      ss << "ALSA";
      hasText = true;
      break;
    default: break;
  }

  if (snapshot.backend != app::core::playback::BackendKind::None)
  {
    ss << (snapshot.exclusiveOutput ? " exclusive" : " shared");
    hasText = true;
  }

  if (snapshot.sourceFormat)
  {
    ss << (hasText ? " | src " : "src ") << formatStream(*snapshot.sourceFormat);
    hasText = true;
  }

  if (snapshot.activeFormat && (!snapshot.sourceFormat || !sameStreamFormat(*snapshot.sourceFormat, *snapshot.activeFormat)))
  {
    ss << (hasText ? " -> pw " : "pw ") << formatStream(*snapshot.activeFormat);
    hasText = true;
  }

  if (snapshot.deviceFormat &&
      ((!snapshot.activeFormat && (!snapshot.sourceFormat || !sameStreamFormat(*snapshot.sourceFormat, *snapshot.deviceFormat))) ||
       (snapshot.activeFormat && !sameStreamFormat(*snapshot.activeFormat, *snapshot.deviceFormat))))
  {
    ss << (hasText ? " -> sink " : "sink ") << formatStream(*snapshot.deviceFormat);
    hasText = true;
  }

  if (snapshot.underrunCount > 0)
  {
    ss << (hasText ? " | " : "") << snapshot.underrunCount << " underruns";
  }

  _playbackLabel.set_text(ss.str());

  auto const hasSinkInspection = snapshot.sinkStatus != app::core::playback::BackendFormatInfo::SinkStatus::None ||
                                 !snapshot.sinkTooltip.empty();
  if (!hasSinkInspection && snapshot.sinkName.empty())
  {
    _sinkLabel.set_text("");
    _sinkLabel.set_visible(false);
    _sinkLabel.set_tooltip_text("");
    _sinkStatusIcon.set_tooltip_text("");
    _sinkStatusIcon.set_visible(false);
    return;
  }

  auto sinkLabelText = snapshot.sinkName.empty() ? std::string{"Sink: detecting..."}
                                                 : std::string{"Sink: "} + snapshot.sinkName;
  _sinkLabel.set_text(sinkLabelText);
  _sinkLabel.set_visible(true);
  _sinkLabel.set_tooltip_text(snapshot.sinkTooltip);

  clearSinkStatusClasses(_sinkStatusIcon);
  switch (snapshot.sinkStatus)
  {
    case app::core::playback::BackendFormatInfo::SinkStatus::Good:
      _sinkStatusIcon.add_css_class("sink-status-good");
      _sinkStatusIcon.set_visible(true);
      break;
    case app::core::playback::BackendFormatInfo::SinkStatus::Warning:
      _sinkStatusIcon.add_css_class("sink-status-warning");
      _sinkStatusIcon.set_visible(true);
      break;
    case app::core::playback::BackendFormatInfo::SinkStatus::Bad:
      _sinkStatusIcon.add_css_class("sink-status-bad");
      _sinkStatusIcon.set_visible(true);
      break;
    case app::core::playback::BackendFormatInfo::SinkStatus::None:
      _sinkStatusIcon.set_visible(false);
      break;
  }

  _sinkStatusIcon.set_tooltip_text(snapshot.sinkTooltip);
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

} // namespace app::ui
