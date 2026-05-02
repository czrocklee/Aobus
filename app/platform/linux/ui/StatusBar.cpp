// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "platform/linux/ui/StatusBar.h"

#include "platform/linux/ui/LayoutConstants.h"
#include "platform/linux/ui/OutputListItems.h"
#include <ao/utility/Log.h>

#include <gdkmm/display.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/stylecontext.h>

#include <giomm/menu.h>
#include <giomm/simpleactiongroup.h>
#include <glibmm/variant.h>

#include <format>
#include <iomanip>
#include <ranges>
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

    std::string formatStream(ao::audio::Format const& format)
    {
      std::stringstream ss;
      constexpr auto kKhzMultiplier = 1000.0;
      ss << (format.sampleRate / kKhzMultiplier) << " kHz · " << static_cast<int>(format.bitDepth)
         << "-bit · "; // NOLINT(readability-magic-numbers)

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
        .output-button {
           border: none;
           background: none;
           box-shadow: none;
           padding: 2px 8px;
           color: @theme_fg_color;
           opacity: 0.8;
           font-weight: bold;
           border-radius: 6px;
        }
        .output-button:hover {
           opacity: 1.0;
           background-color: alpha(@theme_fg_color, 0.1);
        }
        .device-row {
           padding: 6px 16px;
           transition: background 150ms ease;
        }
        .menu-header {
           font-weight: 800;
           font-size: 0.75em;
           padding-top: 12px;
           padding-bottom: 4px;
           padding-left: 12px;
           padding-right: 12px;
           color: @theme_selected_bg_color;
           text-transform: uppercase;
           letter-spacing: 0.12em;
           opacity: 0.7;
        }
        /* Restore clean top spacing for the first header */
        listboxrow:first-child .menu-header {
           padding-top: 8px;
        }
        .selected-row {
           background-color: alpha(@theme_selected_bg_color, 0.15);
           border-left: 4px solid @theme_selected_bg_color;
        }
        .selected-row label {
           color: @theme_selected_bg_color;
           font-weight: bold;
        }
        .sink-status-perfect { color: #A855F7; }
        .sink-status-lossless { color: #10B981; }
        .sink-status-intervention { color: #F59E0B; }
        .sink-status-lossy { color: #6B7280; }
        .sink-status-clipped { color: #EF4444; }
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
      image.remove_css_class("sink-status-perfect");
      image.remove_css_class("sink-status-lossless");
      image.remove_css_class("sink-status-intervention");
      image.remove_css_class("sink-status-lossy");
      image.remove_css_class("sink-status-clipped");
    }
  }

  StatusBar::StatusBar()
    : Gtk::Box{Gtk::Orientation::HORIZONTAL}
  {
    ensureStatusBarCss();

    set_hexpand(true);
    set_valign(Gtk::Align::END);
    set_margin(Layout::kMarginSmall);
    add_css_class("status-bar");

    // Playback details (Left)
    _playbackDetailsBox.set_spacing(Layout::kSpacingLarge);
    _playbackDetailsBox.set_margin_start(Layout::kMarginMedium);
    _playbackDetailsBox.set_margin_end(Layout::kMarginMedium);

    _outputButton.add_css_class("output-button");
    _outputButton.set_label("Output");
    _outputButton.set_tooltip_text("Click to change audio backend or device");

    _outputStore = Gio::ListStore<Glib::Object>::create();
    _outputListBox.set_selection_mode(Gtk::SelectionMode::NONE);
    _outputListBox.set_show_separators(true); // Restore standard separators
    _outputListBox.add_css_class("rich-list");
    _outputListBox.bind_model(_outputStore, sigc::mem_fun(*this, &StatusBar::createOutputWidget));

    _outputListBox.signal_row_activated().connect(
      [this](Gtk::ListBoxRow* row)
      {
        auto const index = row->get_index();
        if (index >= 0 && static_cast<std::size_t>(index) < _outputStore->get_n_items())
        {
          auto item = _outputStore->get_item(index);
          if (auto deviceItem = std::dynamic_pointer_cast<DeviceItem>(item))
          {
            _outputChanged.emit(deviceItem->kind, deviceItem->id);
            _outputPopover.popdown();
          }
        }
      });

    auto* scrolled = Gtk::make_managed<Gtk::ScrolledWindow>();
    scrolled->set_child(_outputListBox);
    scrolled->set_propagate_natural_height(true);
    scrolled->set_min_content_height(kOutputScrolledMinHeight);
    scrolled->set_min_content_width(kOutputScrolledMinWidth);

    _outputPopover.set_child(*scrolled);
    _outputPopover.set_parent(_outputButton);
    _outputPopover.set_autohide(true);
    _outputPopover.set_position(Gtk::PositionType::TOP);

    _outputButton.signal_toggled().connect(
      [this]()
      {
        if (_outputButton.get_active())
        {
          _outputPopover.popup();
        }
        else
        {
          _outputPopover.popdown();
        }
      });

    _outputPopover.signal_closed().connect([this]() { _outputButton.set_active(false); });

    _streamInfoLabel.add_css_class("dim-label");
    _sinkStatusIcon.set_from_icon_name("media-record-symbolic");
    _sinkStatusIcon.set_pixel_size(Layout::kIconSizeXSmall);
    _sinkStatusIcon.set_visible(false);

    _playbackDetailsBox.append(_outputButton);
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
    _importBox.set_spacing(Layout::kSpacingXLarge);
    _importBox.set_visible(false);

    _importLabel.add_css_class("dim-label");
    _importBox.append(_importLabel);

    _importProgressBar.set_valign(Gtk::Align::CENTER);
    _importProgressBar.set_size_request(kImportProgressWidth, -1);
    _importBox.append(_importProgressBar);
    append(_importBox);

    // Info Stack: Toggle between Selection Info and Status Messages
    auto* infoStack = Gtk::make_managed<Gtk::Stack>();
    infoStack->set_transition_type(Gtk::StackTransitionType::SLIDE_UP_DOWN);
    infoStack->set_transition_duration(kTransitionDurationMs);

    _selectionLabel.add_css_class("dim-label");
    _selectionLabel.set_halign(Gtk::Align::END);

    _statusLabel.add_css_class("status-message");
    _statusLabel.set_halign(Gtk::Align::END);

    infoStack->add(_selectionLabel, "info");
    infoStack->add(_statusLabel, "status");
    append(*infoStack);

    // Separator
    auto* sep1 = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL);
    sep1->set_margin_start(Layout::kMarginMedium);
    sep1->set_margin_end(Layout::kMarginMedium);
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

    // Switch stack to status
    if (auto* stack = dynamic_cast<Gtk::Stack*>(_statusLabel.get_parent()))
    {
      stack->set_visible_child("status");
    }

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

    // Switch stack back to info
    if (auto* stack = dynamic_cast<Gtk::Stack*>(_statusLabel.get_parent()))
    {
      stack->set_visible_child("info");
    }
  }

  void StatusBar::setTrackCount(std::size_t count)
  {
    _libraryLabel.set_text(std::format("{} tracks", count));
  }

  void StatusBar::setSelectionInfo(std::size_t count, std::optional<std::chrono::milliseconds> totalDuration)
  {
    if (count == 0)
    {
      _selectionLabel.set_text("");
      return;
    }

    std::string text = std::format("{} {}", count, count == 1 ? "item selected" : "items selected");

    if (totalDuration && totalDuration->count() > 0)
    {
      text += " (" + formatDuration(*totalDuration) + ")";
    }

    _selectionLabel.set_text(text);
  }

  Gtk::Widget* StatusBar::createOutputWidget(Glib::RefPtr<Glib::Object> const& item)
  {
    if (auto backendItem = std::dynamic_pointer_cast<BackendItem>(item))
    {
      auto* header = Gtk::make_managed<Gtk::Label>(backendItem->name);
      header->set_halign(Gtk::Align::FILL);
      header->set_valign(Gtk::Align::CENTER);
      header->set_xalign(0.0); // Left align text while background fills width
      header->add_css_class("menu-header");
      return header;
    }

    if (auto deviceItem = std::dynamic_pointer_cast<DeviceItem>(item))
    {
      auto* rowBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      rowBox->set_spacing(Layout::kSpacingXLarge);
      rowBox->set_valign(Gtk::Align::CENTER);
      rowBox->add_css_class("device-row");

      auto* textBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
      textBox->set_spacing(0); // Explicitly remove gap between lines
      textBox->set_hexpand(true);
      textBox->set_valign(Gtk::Align::CENTER);

      auto* nameLabel = Gtk::make_managed<Gtk::Label>(deviceItem->name);
      nameLabel->set_halign(Gtk::Align::START);
      nameLabel->set_ellipsize(Pango::EllipsizeMode::END);
      textBox->append(*nameLabel);

      if (!deviceItem->description.empty())
      {
        auto* descLabel = Gtk::make_managed<Gtk::Label>(deviceItem->description);
        descLabel->set_halign(Gtk::Align::START);
        descLabel->add_css_class("menu-description");
        descLabel->set_ellipsize(Pango::EllipsizeMode::END);
        textBox->append(*descLabel);
      }

      rowBox->append(*textBox);

      // Mark current device if it matches
      if (deviceItem->active)
      {
        auto* checkIcon = Gtk::make_managed<Gtk::Image>();
        checkIcon->set_from_icon_name("object-select-symbolic");
        checkIcon->set_pixel_size(16);
        rowBox->append(*checkIcon);

        // We can't easily add a CSS class to the ListBoxRow here because we return the child.
        // But we can style the rowBox itself or use a helper.
        rowBox->add_css_class("selected-row");
      }

      return rowBox;
    }

    return nullptr;
  }

  void StatusBar::updateOutputModel(ao::audio::Snapshot const& snapshot)
  {
    _outputStore->remove_all();

    for (auto const& backend : snapshot.availableBackends)
    {
      _outputStore->append(BackendItem::create(backend.kind, backend.displayName));

      for (auto const& device : backend.devices)
      {
        auto item = DeviceItem::create(backend.kind, device);
        item->active = (backend.kind == snapshot.backend && device.id == snapshot.currentDeviceId);
        _outputStore->append(item);
      }
    }
  }

  void StatusBar::updateOutputLabel(ao::audio::Snapshot const& snapshot)
  {
    bool found = false;
    for (auto const& backend : snapshot.availableBackends)
    {
      if (backend.kind == snapshot.backend)
      {
        for (auto const& device : backend.devices)
        {
          if (device.id == snapshot.currentDeviceId)
          {
            _outputButton.set_label(backend.shortName);
            if (_outputButton.get_tooltip_text() != device.displayName)
            {
              _outputButton.set_tooltip_text(device.displayName);
            }
            found = true;
            break;
          }
        }
      }
      if (found)
      {
        break;
      }
    }

    if (!found)
    {
      _outputButton.set_label("Output");
      _outputButton.set_tooltip_text("Click to change audio backend or device");
    }
  }

  void StatusBar::updatePlaybackStatusLabels(ao::audio::Snapshot const& snapshot)
  {
    if (snapshot.transport == ao::audio::Transport::Idle)
    {
      _nowPlayingLabel.set_text("");
      _streamInfoLabel.set_text("");
      _sinkStatusIcon.set_visible(false);
      _lastTooltipText.clear();
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

    // Source Format from Decoder Node
    std::stringstream ss;
    bool formatFound = false;
    for (auto const& node : snapshot.flow.nodes)
    {
      if (node.type == ao::audio::flow::NodeType::Decoder && node.format)
      {
        ss << formatStream(*node.format);
        formatFound = true;
        break;
      }
    }

    if (snapshot.underrunCount > 0)
    {
      if (formatFound)
      {
        ss << " | ";
      }
      ss << snapshot.underrunCount << " underruns";
    }

    _streamInfoLabel.set_text(ss.str());

    updatePlaybackTooltip(snapshot);

    // Update status icon
    clearSinkStatusClasses(_sinkStatusIcon);
    _sinkStatusIcon.set_visible(true);

    using Quality = ao::audio::Quality;
    switch (snapshot.quality)
    {
      case Quality::BitwisePerfect: _sinkStatusIcon.add_css_class("sink-status-perfect"); break;
      case Quality::LosslessPadded:
      case Quality::LosslessFloat: _sinkStatusIcon.add_css_class("sink-status-lossless"); break;
      case Quality::LinearIntervention: _sinkStatusIcon.add_css_class("sink-status-intervention"); break;
      case Quality::LossySource: _sinkStatusIcon.add_css_class("sink-status-lossy"); break;
      case Quality::Clipped: _sinkStatusIcon.add_css_class("sink-status-clipped"); break;
      case Quality::Unknown: _sinkStatusIcon.set_visible(false); break;
    }
  }

  void StatusBar::updatePlaybackTooltip(ao::audio::Snapshot const& snapshot)
  {
    // Tooltip: Build dynamic representation of the path from the graph (TOTAL ORDER)
    std::stringstream tt;
    tt << "Audio Pipeline:\n";

    {
      std::string currentId = "rs-decoder";
      std::set<std::string> visited;
      while (!currentId.empty() && !visited.contains(currentId))
      {
        visited.insert(currentId);
        auto it = std::ranges::find(snapshot.flow.nodes, currentId, &ao::audio::flow::Node::id);

        if (it == snapshot.flow.nodes.end())
        {
          break;
        }
        auto const& node = *it;
        tt << "• ";
        switch (node.type)
        {
          case ao::audio::flow::NodeType::Decoder: tt << "[Source] "; break;
          case ao::audio::flow::NodeType::Engine: tt << "[Engine] "; break;
          case ao::audio::flow::NodeType::Stream: tt << "[Stream] "; break;
          case ao::audio::flow::NodeType::Intermediary: tt << "[Filter] "; break;
          case ao::audio::flow::NodeType::Sink: tt << "[Device] "; break;
          case ao::audio::flow::NodeType::ExternalSource: tt << "[Other Source] "; break;
        }
        tt << node.name;
        if (node.format)
        {
          tt << " (" << formatStream(*node.format) << ")";
        }
        if (node.volumeNotUnity)
        {
          tt << " [Vol Control]";
        }
        if (node.isMuted)
        {
          tt << " [Muted]";
        }
        tt << "\n";

        std::string nextId;
        for (auto const& link : snapshot.flow.connections)
        {
          if (link.isActive && link.sourceId == currentId)
          {
            nextId = link.destId;
            break;
          }
        }
        currentId = nextId;
      }
    }

    if (!snapshot.qualityTooltip.empty())
    {
      tt << "\n" << snapshot.qualityTooltip;
    }

    auto const finalTooltip = tt.str();
    if (finalTooltip != _lastTooltipText)
    {
      APP_LOG_DEBUG("StatusBar: Updating tooltip (length={})", finalTooltip.length());
      _playbackDetailsBox.set_tooltip_text(finalTooltip);
      _streamInfoLabel.set_tooltip_text(finalTooltip);
      _sinkStatusIcon.set_tooltip_text(finalTooltip);
      _lastTooltipText = finalTooltip;
    }
  }

  void StatusBar::setPlaybackDetails(ao::audio::Snapshot const& snapshot)
  {
    // Skip update if nothing visible has changed
    if (snapshot.transport == _lastPlaybackState.transport && snapshot.backend == _lastPlaybackState.backend &&
        snapshot.trackTitle == _lastPlaybackState.title && snapshot.trackArtist == _lastPlaybackState.artist &&
        snapshot.underrunCount == _lastPlaybackState.underrunCount && snapshot.quality == _lastPlaybackState.quality &&
        snapshot.qualityTooltip == _lastPlaybackState.qualityTooltip && snapshot.flow == _lastPlaybackState.flow &&
        snapshot.currentDeviceId == _lastPlaybackState.currentDeviceId &&
        snapshot.availableBackends == _lastPlaybackState.availableBackends)
    {
      return;
    }

    // Detect significant changes
    bool const backendsChanged = (snapshot.availableBackends != _lastPlaybackState.availableBackends);
    bool const deviceChanged = (snapshot.currentDeviceId != _lastPlaybackState.currentDeviceId);
    bool const backendKindChanged = (snapshot.backend != _lastPlaybackState.backend);

    // Update state cache
    _lastPlaybackState = {.transport = snapshot.transport,
                          .backend = snapshot.backend,
                          .title = snapshot.trackTitle,
                          .artist = snapshot.trackArtist,
                          .underrunCount = snapshot.underrunCount,
                          .quality = snapshot.quality,
                          .qualityTooltip = snapshot.qualityTooltip,
                          .flow = snapshot.flow,
                          .currentDeviceId = snapshot.currentDeviceId,
                          .availableBackends = snapshot.availableBackends};

    // Update output model if backends or device changed
    if (backendsChanged || deviceChanged || backendKindChanged)
    {
      updateOutputModel(snapshot);
    }

    // Always update output label and status labels (they might depend on minor state changes)
    updateOutputLabel(snapshot);
    updatePlaybackStatusLabels(snapshot);
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
