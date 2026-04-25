// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "platform/linux/ui/StatusBar.h"

#include <gdkmm/display.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/stylecontext.h>

#include <giomm/menu.h>
#include <giomm/simpleactiongroup.h>
#include <glibmm/variant.h>

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
        .backend-button {
           border: none;
           background: none;
           box-shadow: none;
           padding: 2px 6px;
           color: @theme_fg_color;
           opacity: 0.7;
           font-weight: bold;
        }
        .backend-button:hover {
           opacity: 1.0;
           background-color: alpha(@theme_fg_color, 0.1);
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

    // Status label (Far Left)
    _statusLabel.set_halign(Gtk::Align::START);
    _statusLabel.add_css_class("status-message");
    _statusLabel.set_visible(false);
    append(_statusLabel);

    // Playback details (Left)
    _playbackDetailsBox.set_spacing(8);
    _playbackDetailsBox.set_margin_start(12);
    _playbackDetailsBox.set_margin_end(12);
    
    _backendButton.add_css_class("backend-button");
    _backendButton.set_label("PipeWire");
    _backendButton.set_tooltip_text("Click to change audio backend");

    auto actionGroup = Gio::SimpleActionGroup::create();
    actionGroup->add_action_with_parameter("set-backend", Glib::VARIANT_TYPE_STRING,
      [this](const Glib::VariantBase& value) {
        auto name = Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(value).get();
        if (name == "pipewire") _backendChanged.emit(app::core::playback::BackendKind::PipeWire);
        else if (name == "alsa") _backendChanged.emit(app::core::playback::BackendKind::AlsaExclusive);
      });
    insert_action_group("status", actionGroup);

    auto menu = Gio::Menu::create();
    menu->append("PipeWire", "status.set-backend('pipewire')");
    menu->append("ALSA (Exclusive)", "status.set-backend('alsa')");
    _backendButton.set_menu_model(menu);

    _streamInfoLabel.add_css_class("dim-label");
    _sinkStatusIcon.set_from_icon_name("media-record-symbolic");
    _sinkStatusIcon.set_pixel_size(12);
    _sinkStatusIcon.set_visible(false);
    
    _playbackDetailsBox.append(_backendButton);
    _playbackDetailsBox.append(_streamInfoLabel);
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
    if (snapshot.state == app::core::playback::TransportState::Idle)
    {
      _nowPlayingLabel.set_text("");
      _streamInfoLabel.set_text("");
      _backendButton.set_label("PipeWire"); // Default
      _sinkStatusIcon.set_visible(false);
      return;
    }

    // Artist - Title
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

    // Backend Label
    switch (snapshot.backend)
    {
      case app::core::playback::BackendKind::PipeWire: _backendButton.set_label("PipeWire"); break;
      case app::core::playback::BackendKind::AlsaExclusive: _backendButton.set_label("ALSA (Exclusive)"); break;
      default: _backendButton.set_label("None"); break;
    }

    // Source Format from Decoder Node
    std::stringstream ss;
    bool formatFound = false;
    for (auto const& node : snapshot.graph.nodes)
    {
      if (node.type == app::core::playback::AudioNodeType::Decoder && node.format)
      {
        ss << formatStream(*node.format);
        formatFound = true;
        break;
      }
    }

    if (snapshot.underrunCount > 0)
    {
      if (formatFound) ss << " | ";
      ss << snapshot.underrunCount << " underruns";
    }

    _streamInfoLabel.set_text(ss.str());

    // Tooltip: Build dynamic representation of the path from the graph
    std::stringstream tt;
    tt << "Audio Pipeline:\n";
    
    for (auto const& node : snapshot.graph.nodes)
    {
      tt << "• ";
      switch (node.type)
      {
        case app::core::playback::AudioNodeType::Decoder: tt << "[Source] "; break;
        case app::core::playback::AudioNodeType::Engine: tt << "[Engine] "; break;
        case app::core::playback::AudioNodeType::Stream: tt << "[Stream] "; break;
        case app::core::playback::AudioNodeType::Intermediary: tt << "[Filter] "; break;
        case app::core::playback::AudioNodeType::Sink: tt << "[Device] "; break;
        case app::core::playback::AudioNodeType::ExternalSource: tt << "[Other Source] "; break;
      }
      tt << node.name;
      if (node.format) tt << " (" << formatStream(*node.format) << ")";
      if (node.volumeNotUnity) tt << " [Vol Control]";
      if (node.isMuted) tt << " [Muted]";
      tt << "\n";
    }

    if (!snapshot.qualityTooltip.empty())
    {
      tt << "\n" << snapshot.qualityTooltip;
    }

    _streamInfoLabel.set_tooltip_text(tt.str());
    _sinkStatusIcon.set_tooltip_text(tt.str());

    // Update status icon
    clearSinkStatusClasses(_sinkStatusIcon);
    _sinkStatusIcon.set_visible(true);
    
    using AudioQuality = app::core::playback::AudioQuality;
    switch (snapshot.quality)
    {
      case AudioQuality::BitPerfect:
        _sinkStatusIcon.add_css_class("sink-status-good");
        break;
      case AudioQuality::Lossless:
      case AudioQuality::Resampled:
        _sinkStatusIcon.add_css_class("sink-status-warning");
        break;
      case AudioQuality::Mixed:
      case AudioQuality::Lossy:
        _sinkStatusIcon.add_css_class("sink-status-bad");
        break;
      case AudioQuality::Unknown:
        _sinkStatusIcon.set_visible(false);
        break;
    }
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
