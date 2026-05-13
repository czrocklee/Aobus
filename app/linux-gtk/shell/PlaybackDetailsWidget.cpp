// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "shell/PlaybackDetailsWidget.h"
#include "layout/LayoutConstants.h"
#include <ao/utility/Log.h>
#include <gdkmm/display.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/stylecontext.h>
#include <runtime/AppSession.h>
#include <runtime/PlaybackService.h>
#include <runtime/StateTypes.h>

#include <format>
#include <ranges>
#include <unordered_set>

namespace ao::gtk
{
  namespace
  {
    void ensurePlaybackDetailsCss()
    {
      static auto const provider = Gtk::CssProvider::create();
      static bool initialized = false;

      if (!initialized)
      {
        provider->load_from_data(R"(
          .sink-status-perfect { color: #A855F7; }
          .sink-status-lossless { color: #10B981; }
          .sink-status-intervention { color: #F59E0B; }
          .sink-status-lossy { color: #6B7280; }
          .sink-status-clipped { color: #EF4444; }
        )");

        if (auto display = Gdk::Display::get_default(); display)
        {
          Gtk::StyleContext::add_provider_for_display(display, provider, GTK_STYLE_PROVIDER_PRIORITY_USER);
        }
        initialized = true;
      }
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

    void clearSinkStatusClasses(Gtk::Image& image)
    {
      image.remove_css_class("sink-status-perfect");
      image.remove_css_class("sink-status-lossless");
      image.remove_css_class("sink-status-intervention");
      image.remove_css_class("sink-status-lossy");
      image.remove_css_class("sink-status-clipped");
    }
  }

  PlaybackDetailsWidget::PlaybackDetailsWidget(ao::rt::AppSession& session)
    : _session{session}
  {
    ensurePlaybackDetailsCss();
    _container.set_spacing(Layout::kSpacingLarge);
    _container.set_margin_start(Layout::kMarginMedium);
    _container.set_margin_end(Layout::kMarginMedium);

    _streamInfoLabel.add_css_class("dim-label");
    _sinkStatusIcon.set_from_icon_name("media-record-symbolic");
    _sinkStatusIcon.set_pixel_size(Layout::kIconSizeXSmall);
    _sinkStatusIcon.set_visible(false);

    _container.append(_streamInfoLabel);
    _container.append(_sinkStatusIcon);

    _startedSub = _session.playback().onStarted([this] { updateState(); });
    _pausedSub = _session.playback().onPaused([this] { updateState(); });
    _idleSub = _session.playback().onIdle([this] { updateState(); });
    _stoppedSub = _session.playback().onStopped([this] { updateState(); });
    _outputChangedSub = _session.playback().onOutputChanged([this](auto const&) { updateState(); });
    _qualityChangedSub = _session.playback().onQualityChanged([this](auto const&) { updateState(); });

    updateState();
  }

  PlaybackDetailsWidget::~PlaybackDetailsWidget() = default;

  void PlaybackDetailsWidget::updateState()
  {
    auto const& state = _session.playback().state();

    if (state.transport == ao::audio::Transport::Idle)
    {
      _streamInfoLabel.set_text(state.ready ? "" : "Connecting to audio engine...");
      _sinkStatusIcon.set_visible(false);
      _lastTooltipText.clear();
      return;
    }

    // Source Format from Decoder Node
    auto const it = std::ranges::find_if(state.flow.nodes,
                                         [](auto const& node)
                                         { return node.type == ao::audio::flow::NodeType::Decoder && node.optFormat; });

    auto const info = (it != state.flow.nodes.end() && it->optFormat) ? formatStream(*it->optFormat) : std::string{};
    _streamInfoLabel.set_text(info);

    updateTooltip(state);

    // Update status icon
    clearSinkStatusClasses(_sinkStatusIcon);
    _sinkStatusIcon.set_visible(true);

    using Quality = ao::audio::Quality;

    switch (state.quality)
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

  void PlaybackDetailsWidget::updateTooltip(ao::rt::PlaybackState const& state)
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
        auto const it = std::ranges::find(state.flow.nodes, currentId, &ao::audio::flow::Node::id);

        if (it == state.flow.nodes.end())
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
          state.flow.connections, [&](auto const& link) { return link.isActive && link.sourceId == currentId; });
        currentId = (linkIt != state.flow.connections.end()) ? linkIt->destId : "";
      }
    }

    if (!state.qualityTooltip.empty())
    {
      std::format_to(std::back_inserter(tooltip), "\n{}", state.qualityTooltip);
    }

    if (tooltip != _lastTooltipText)
    {
      _container.set_tooltip_text(tooltip);
      _streamInfoLabel.set_tooltip_text(tooltip);
      _sinkStatusIcon.set_tooltip_text(tooltip);
      _lastTooltipText = tooltip;
    }
  }
} // namespace ao::gtk
