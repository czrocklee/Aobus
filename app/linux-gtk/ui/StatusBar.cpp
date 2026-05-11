// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "StatusBar.h"
#include "LayoutConstants.h"
#include "OutputListItems.h"
#include "SvgTemplate.h"
#include <ao/utility/Log.h>
#include <runtime/AppSession.h>
#include <runtime/LibraryMutationService.h>
#include <runtime/ListSourceStore.h>
#include <runtime/NotificationService.h>
#include <runtime/PlaybackService.h>
#include <runtime/StateTypes.h>
#include <runtime/ViewService.h>
#include <runtime/WorkspaceService.h>

#include "ui/ThemeBus.h"
#include <gdkmm/display.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/stylecontext.h>

#include <giomm/menu.h>
#include <giomm/simpleactiongroup.h>
#include <glibmm/variant.h>

#include <format>
#include <iomanip>
#include <iterator>
#include <ranges>
#include <unordered_set>

namespace ao::gtk
{
  namespace
  {
    std::string formatDuration(std::chrono::milliseconds ms)
    {
      auto const totalSeconds = std::chrono::duration_cast<std::chrono::seconds>(ms).count();
      auto const hours = totalSeconds / 3600;
      auto const minutes = (totalSeconds % 3600) / 60;
      auto const seconds = totalSeconds % 60;

      if (hours > 0)
      {
        return std::format("{}:{:02}:{:02}", hours, minutes, seconds);
      }

      return std::format("{}:{:02}", minutes, seconds);
    }

    std::string formatStream(ao::audio::Format const& format)
    {
      constexpr auto kKhzMultiplier = 1000.0;
      auto const channelsText = [&]
      {
        if (format.channels == 1)
        {
          return std::string{"Mono"};
        }

        if (format.channels == 2)
        {
          return std::string{"Stereo"};
        }

        return std::format("{} ch", format.channels);
      }();

      return std::format("{:.1f} kHz · {}-bit · {}", format.sampleRate / kKhzMultiplier, format.bitDepth, channelsText);
    }

    void ensureStatusBarCss(bool force = false)
    {
      static auto const provider = Gtk::CssProvider::create();
      static bool initialized = false;

      if (!initialized || force)
      {
        if (force)
        {
          if (auto const display = Gdk::Display::get_default(); display)
          {
            Gtk::StyleContext::remove_provider_for_display(display, provider);
          }
        }

        provider->load_from_data(R"(
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
          /* No direct cursor property in standard GTK CSS for Label */
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

        if (!initialized || force)
        {
          if (auto display = Gdk::Display::get_default(); display)
          {
            Gtk::StyleContext::add_provider_for_display(display, provider, GTK_STYLE_PROVIDER_PRIORITY_USER);
          }

          initialized = true;
        }
      }
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

  StatusBar::StatusBar(ao::app::AppSession& session)
    : Gtk::Box{Gtk::Orientation::HORIZONTAL}, _session{session}
  {
    ensureStatusBarCss();

    signalThemeRefresh().connect(
      [this]
      {
        APP_LOG_INFO("Executing theme refresh for StatusBar...");
        ensureStatusBarCss(true);
        queue_draw();
      });

    set_hexpand(true);
    set_valign(Gtk::Align::END);
    set_margin(Layout::kMarginSmall);
    add_css_class("status-bar");

    // Self-wire: events drive state reads from store snapshot
    _startedSub = _session.playback().onStarted([this] { setPlaybackState(_session.playback().state()); });
    _pausedSub = _session.playback().onPaused([this] { setPlaybackState(_session.playback().state()); });
    _idleSub = _session.playback().onIdle([this] { setPlaybackState(_session.playback().state()); });
    _stoppedSub = _session.playback().onStopped([this] { setPlaybackState(_session.playback().state()); });
    _outputChangedSub =
      _session.playback().onOutputChanged([this](auto const&) { setPlaybackState(_session.playback().state()); });
    _qualityChangedSub =
      _session.playback().onQualityChanged([this](auto const&) { setPlaybackState(_session.playback().state()); });

    _notificationPostedSub = _session.notifications().onPosted(
      [this](auto)
      {
        auto const feed = _session.notifications().feed();
        if (!feed.entries.empty())
        {
          auto const& latest = feed.entries.back();
          auto const duration = latest.optTimeout.value_or(std::chrono::seconds{5});
          showMessage(latest.message, std::chrono::duration_cast<std::chrono::seconds>(duration));
        }
      });

    // Populate initial state from snapshot (one-shot, no subscription)
    applyInitialState();

    // Self-wire: selection changes update the selection label
    _selectionChangedSub =
      _session.views().onSelectionChanged([this](auto const& ev) { setSelectionInfo(ev.selection.size()); });

    // Self-wire: import completed reloads track count
    _importCompletedSub = _session.mutation().onImportCompleted(
      [this](auto)
      {
        clearImportProgress();
        setTrackCount(_session.sources().allTracks().size());
      });

    // Playback details (Left)
    _playbackDetailsBox.set_spacing(Layout::kSpacingLarge);
    _playbackDetailsBox.set_margin_start(Layout::kMarginMedium);
    _playbackDetailsBox.set_margin_end(Layout::kMarginMedium);

    _streamInfoLabel.add_css_class("dim-label");
    _sinkStatusIcon.set_from_icon_name("media-record-symbolic");
    _sinkStatusIcon.set_pixel_size(Layout::kIconSizeXSmall);
    _sinkStatusIcon.set_visible(false);

    _playbackDetailsBox.append(_streamInfoLabel);
    _playbackDetailsBox.append(_sinkStatusIcon);
    append(_playbackDetailsBox);

    // Spacer to push now playing to the center
    auto* const spacer1 = Gtk::make_managed<Gtk::Box>();
    spacer1->set_hexpand(true);
    append(*spacer1);

    // Now playing (Center)
    _nowPlayingLabel.add_css_class("now-playing-label");
    _nowPlayingLabel.add_css_class("clickable-label");
    _nowPlayingLabel.set_tooltip_text("Click to show playing list");

    auto const clickGesture = Gtk::GestureClick::create();
    clickGesture->signal_pressed().connect([this](int, double, double) { _session.playback().revealPlayingTrack(); });

    _nowPlayingLabel.add_controller(clickGesture);
    _nowPlayingLabel.set_cursor(Gdk::Cursor::create("pointer"));
    append(_nowPlayingLabel);

    // Spacer to push info to the right
    auto* const spacer2 = Gtk::make_managed<Gtk::Box>();
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
    auto* const infoStack = Gtk::make_managed<Gtk::Stack>();
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
    auto* const sep1 = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL);
    sep1->set_margin_start(Layout::kMarginMedium);
    sep1->set_margin_end(Layout::kMarginMedium);
    append(*sep1);

    // Library info (Far Right)
    _libraryLabel.add_css_class("dim-label");
    append(_libraryLabel);

    setTrackCount(0);
  }

  StatusBar::~StatusBar() = default;

  void StatusBar::showMessage(std::string_view message, std::chrono::seconds duration)
  {
    if (_timerConnection)
    {
      _timerConnection.disconnect();
    }

    _statusLabel.set_text(std::string{message});

    // Switch stack to status
    if (auto* const stack = dynamic_cast<Gtk::Stack*>(_statusLabel.get_parent()))
    {
      stack->set_visible_child("status");
    }

    _timerConnection = Glib::signal_timeout().connect_seconds(
      [this]
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

    auto text = std::format("{} {}", count, count == 1 ? "item selected" : "items selected");

    if (totalDuration && totalDuration->count() > 0)
    {
      text += std::format(" ({})", formatDuration(*totalDuration));
    }

    _selectionLabel.set_text(text);
  }

  void StatusBar::updatePlaybackStatusLabels(ao::audio::Player::Status const& status)
  {
    if (status.engine.transport == ao::audio::Transport::Idle)
    {
      _nowPlayingLabel.set_text("");
      _streamInfoLabel.set_text(status.isReady ? "" : "Connecting to audio engine...");
      _sinkStatusIcon.set_visible(false);
      _lastTooltipText.clear();
      return;
    }

    // Artist - Title
    if (!status.trackTitle.empty())
    {
      if (!status.trackArtist.empty())
      {
        _nowPlayingLabel.set_text(std::format("{} - {}", status.trackArtist, status.trackTitle));
      }
      else
      {
        _nowPlayingLabel.set_text(status.trackTitle);
      }
    }
    else
    {
      _nowPlayingLabel.set_text("");
    }

    // Source Format from Decoder Node
    auto const statusText = [&]
    {
      auto const it = std::ranges::find_if(
        status.flow.nodes,
        [](auto const& node) { return node.type == ao::audio::flow::NodeType::Decoder && node.optFormat; });

      auto info = (it != status.flow.nodes.end()) ? formatStream(*it->optFormat) : std::string{};

      if (status.engine.underrunCount == 0)
      {
        return info;
      }

      if (info.empty())
      {
        return std::format("{} underruns", status.engine.underrunCount);
      }

      return std::format("{} | {} underruns", info, status.engine.underrunCount);
    }();

    _streamInfoLabel.set_text(statusText);

    updatePlaybackTooltip(status);

    // Update status icon
    clearSinkStatusClasses(_sinkStatusIcon);
    _sinkStatusIcon.set_visible(true);

    using Quality = ao::audio::Quality;
    switch (status.quality)
    {
      case Quality::BitwisePerfect:
      case Quality::LosslessPadded: _sinkStatusIcon.add_css_class("sink-status-perfect"); break;
      case Quality::LosslessFloat: _sinkStatusIcon.add_css_class("sink-status-lossless"); break;
      case Quality::LinearIntervention: _sinkStatusIcon.add_css_class("sink-status-intervention"); break;
      case Quality::LossySource: _sinkStatusIcon.add_css_class("sink-status-lossy"); break;
      case Quality::Clipped: _sinkStatusIcon.add_css_class("sink-status-clipped"); break;
      case Quality::Unknown: _sinkStatusIcon.set_visible(false); break;
    }
  }

  void StatusBar::updatePlaybackTooltip(ao::audio::Player::Status const& status)
  {
    auto tooltip = std::string{"Audio Pipeline:\n"};

    auto const nodeTypeString = [](ao::audio::flow::NodeType type)
    {
      using Type = ao::audio::flow::NodeType;

      switch (type)
      {
        case Type::Decoder: return "[Source]";
        case Type::Engine: return "[Engine]";
        case Type::Stream: return "[Stream]";
        case Type::Intermediary: return "[Filter]";
        case Type::Sink: return "[Device]";
        case Type::ExternalSource: return "[Other Source]";
      }

      return "[Unknown]";
    };

    {
      auto currentId = std::string{"ao-decoder"};
      auto visited = std::unordered_set<std::string>{};

      while (!currentId.empty() && !visited.contains(currentId))
      {
        visited.insert(currentId);
        auto const it = std::ranges::find(status.flow.nodes, currentId, &ao::audio::flow::Node::id);

        if (it == status.flow.nodes.end())
        {
          break;
        }

        auto const& node = *it;
        std::format_to(std::back_inserter(tooltip), "• {} {}", nodeTypeString(node.type), node.name);

        if (node.optFormat)
        {
          std::format_to(std::back_inserter(tooltip), " ({})", formatStream(*node.optFormat));
        }

        if (node.volumeNotUnity)
        {
          tooltip += " [Vol Control]";
        }

        if (node.isMuted)
        {
          tooltip += " [Muted]";
        }

        tooltip += "\n";

        auto const linkIt = std::ranges::find_if(
          status.flow.connections, [&](auto const& link) { return link.isActive && link.sourceId == currentId; });

        currentId = (linkIt != status.flow.connections.end()) ? linkIt->destId : "";
      }
    }

    if (!status.qualityTooltip.empty())
    {
      std::format_to(std::back_inserter(tooltip), "\n{}", status.qualityTooltip);
    }

    if (tooltip != _lastTooltipText)
    {
      APP_LOG_DEBUG("StatusBar: Updating tooltip (length={})", tooltip.length());
      _playbackDetailsBox.set_tooltip_text(tooltip);
      _streamInfoLabel.set_tooltip_text(tooltip);
      _sinkStatusIcon.set_tooltip_text(tooltip);
      _lastTooltipText = tooltip;
    }
  }

  void StatusBar::setPlaybackDetails(ao::audio::Player::Status const& status)
  {
    // Skip update if nothing visible has changed
    if (status.engine.transport == _lastPlaybackState.engine.transport &&
        status.trackTitle == _lastPlaybackState.title && status.trackArtist == _lastPlaybackState.artist &&
        status.engine.underrunCount == _lastPlaybackState.underrunCount &&
        status.quality == _lastPlaybackState.quality && status.qualityTooltip == _lastPlaybackState.qualityTooltip &&
        status.flow == _lastPlaybackState.flow)
    {
      return;
    }

    // Update state cache
    _lastPlaybackState = {.engine = status.engine,
                          .title = status.trackTitle,
                          .artist = status.trackArtist,
                          .underrunCount = status.engine.underrunCount,
                          .quality = status.quality,
                          .qualityTooltip = status.qualityTooltip,
                          .flow = status.flow};

    // Always update status labels (they might depend on minor state changes)
    updatePlaybackStatusLabels(status);
  }

  void StatusBar::setPlaybackState(ao::app::PlaybackState const& state)
  {
    ao::audio::Player::Status status;
    status.engine.transport = state.transport;
    status.engine.backendId = state.selectedOutput.backendId;
    status.engine.profileId = state.selectedOutput.profileId;
    status.engine.currentDeviceId = state.selectedOutput.deviceId;
    status.flow = state.flow;
    status.quality = state.quality;
    status.isReady = state.ready;
    status.trackTitle = state.trackTitle;
    status.trackArtist = state.trackArtist;

    updatePlaybackStatusLabels(status);
  }

  void StatusBar::applyInitialState()
  {
    setPlaybackState(_session.playback().state());
  }

  void StatusBar::setImportProgress(double fraction, std::string_view info)
  {
    _importBox.set_visible(true);
    _importProgressBar.set_fraction(fraction);
    _importLabel.set_text(std::string{info});
    _selectionLabel.set_visible(false);
  }

  void StatusBar::clearImportProgress()
  {
    _importBox.set_visible(false);
    _selectionLabel.set_visible(true);
  }
} // namespace ao::gtk
