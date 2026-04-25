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
      ss << (format.sampleRate / 1000.0) << " kHz · " << static_cast<int>(format.bitDepth) << "-bit · ";
      
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

    bool sameStreamFormat(app::core::playback::StreamFormat const& lhs,
                          app::core::playback::StreamFormat const& rhs) noexcept
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
        .status-bar {
          min-height: 24px;
          padding-top: 1px;
          padding-bottom: 1px;
        }
        .now-playing-label {
          transition: all 200ms ease-in-out;
          padding: 2px 12px;
          border-radius: 6px;
          color: @theme_fg_color;
          opacity: 0.85;
        }
        .now-playing-label:hover {
          background-color: alpha(@theme_selected_bg_color, 0.15);
          color: @theme_selected_bg_color;
          opacity: 1.0;
        }
        .now-playing-label:active {
          background-color: alpha(@theme_selected_bg_color, 0.25);
          opacity: 0.7;
        }
        .clickable-label {
          cursor: pointer;
        }
        .sink-status-good { color: #34a853; }
        .sink-status-warning { color: #fbbc04; }
        .sink-status-bad { color: #ea4335; }
      )");

        if (auto display = Gdk::Display::get_default(); display)
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
    set_margin(6);
    add_css_class("status-bar");

    // Add a subtle top border via a separator if needed,
    // but for now we just use margins to increase height.

    // Status label (Far Left)
    _statusLabel.set_halign(Gtk::Align::START);
    _statusLabel.add_css_class("status-message");
    _statusLabel.set_visible(false);
    append(_statusLabel);

    // Playback details (Left)
    _playbackDetailsBox.set_spacing(8);
    _playbackDetailsBox.set_margin_start(12);
    _playbackDetailsBox.set_margin_end(12);
    _playbackLabel.add_css_class("dim-label");
    _sinkStatusIcon.set_from_icon_name("media-record-symbolic");
    _sinkStatusIcon.set_pixel_size(12);
    _sinkStatusIcon.set_visible(false);
    _playbackDetailsBox.append(_playbackLabel);
    _playbackDetailsBox.append(_sinkStatusIcon);
    append(_playbackDetailsBox);

    // Spacer to push now playing to the center
    auto* spacer1 = Gtk::make_managed<Gtk::Box>();
    spacer1->set_hexpand(true);
    append(*spacer1);

    // Now playing (Center)
    _nowPlayingLabel.add_css_class("now-playing-label");
    _nowPlayingLabel.add_css_class("clickable-label");
    _nowPlayingLabel.set_tooltip_text("Click to show playing list");
    auto clickGesture = Gtk::GestureClick::create();
    clickGesture->signal_pressed().connect([this](int, double, double) { _nowPlayingClicked.emit(); });
    _nowPlayingLabel.add_controller(clickGesture);
    append(_nowPlayingLabel);

    // Spacer to push info to the right
    auto* spacer2 = Gtk::make_managed<Gtk::Box>();
    spacer2->set_hexpand(true);
    append(*spacer2);

    // Import box (Right, hidden by default)
    _importBox.set_orientation(Gtk::Orientation::HORIZONTAL);
    _importBox.set_spacing(12);
    _importBox.set_visible(false);

    _importLabel.add_css_class("dim-label");
    _importBox.append(_importLabel);

    _importProgressBar.set_valign(Gtk::Align::CENTER);
    _importProgressBar.set_size_request(200, -1);
    _importBox.append(_importProgressBar);
    append(_importBox);

    // Selection info
    _selectionLabel.add_css_class("dim-label");
    append(_selectionLabel);

    // Separator
    auto* sep1 = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL);
    sep1->set_margin_start(6);
    sep1->set_margin_end(6);
    append(*sep1);

    // Library info (Far Right)
    _libraryLabel.add_css_class("dim-label");
    append(_libraryLabel);

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
      [this]()
      {
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
    if (snapshot.state == app::core::playback::TransportState::Idle ||
        (!snapshot.sourceFormat && !snapshot.activeFormat))
    {
      _nowPlayingLabel.set_text("");
      _playbackLabel.set_text("");
      _playbackLabel.set_tooltip_text("");
      return;
    }

    // Center: Artist - Title
    
    if (!snapshot.trackTitle.empty())
    {
      if (!snapshot.trackArtist.empty())
      {
        _nowPlayingLabel.set_text(snapshot.trackArtist + " - " + snapshot.trackTitle);
      }
      else
      {
        _nowPlayingLabel.set_text(snapshot.trackTitle);
      }
    }
    else
    {
      _nowPlayingLabel.set_text("");
    }

    // Right: Backend + SW info (Compact)
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
      ss << (hasText ? " | " : "") << formatStream(*snapshot.sourceFormat);
    }

    if (snapshot.underrunCount > 0)
    {
      ss << " | " << snapshot.underrunCount << " underruns";
    }

    _playbackLabel.set_text(ss.str());

    // Tooltip: Full chain (src -> pw -> sink)
    std::stringstream tt;
    
    if (snapshot.sourceFormat)
    {
      tt << "Source: " << formatStream(*snapshot.sourceFormat) << "\n";
    }

    if (snapshot.activeFormat &&
        (!snapshot.sourceFormat || !sameStreamFormat(*snapshot.sourceFormat, *snapshot.activeFormat)))
    {
      tt << "PipeWire: " << formatStream(*snapshot.activeFormat) << "\n";
    }

    if (snapshot.deviceFormat &&
        ((!snapshot.activeFormat &&
          (!snapshot.sourceFormat || !sameStreamFormat(*snapshot.sourceFormat, *snapshot.deviceFormat))) ||
         (snapshot.activeFormat && !sameStreamFormat(*snapshot.activeFormat, *snapshot.deviceFormat))))
    {
      tt << "Sink: " << formatStream(*snapshot.deviceFormat) << "\n";
    }

    if (!snapshot.sinkName.empty())
    {
      tt << "Device: " << snapshot.sinkName << "\n";
    }

    if (!snapshot.sinkTooltip.empty())
    {
      tt << "\n" << snapshot.sinkTooltip;
    }

    _playbackLabel.set_tooltip_text(tt.str());

    // Update status icon
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
      case app::core::playback::BackendFormatInfo::SinkStatus::None: _sinkStatusIcon.set_visible(false); break;
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
