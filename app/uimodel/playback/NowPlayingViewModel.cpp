// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ao/audio/Backend.h"
#include "ao/audio/Format.h"
#include "ao/audio/flow/Graph.h"
#include "ao/rt/PlaybackService.h"
#include "ao/rt/StateTypes.h"
#include <ao/rt/TrackField.h>
#include <ao/uimodel/playback/NowPlayingViewModel.h>

#include <algorithm>
#include <format>
#include <functional>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace ao::uimodel::playback
{
  namespace
  {
    std::string quoteExpressionString(std::string_view value)
    {
      if (!value.contains('"'))
      {
        return std::format("\"{}\"", value);
      }

      if (!value.contains('\''))
      {
        return std::format("'{}'", value);
      }

      auto sanitized = std::string{value};
      std::ranges::replace(sanitized, '"', '\'');
      return std::format("\"{}\"", sanitized);
    }

    std::string formatStream(audio::Format const& format)
    {
      constexpr auto kKhzMultiplier = 1000.0;
      auto const channelsText = [&] -> std::string
      {
        if (format.channels == 1)
        {
          return "Mono";
        }

        if (format.channels == 2)
        {
          return "Stereo";
        }

        return std::format("{} ch", format.channels);
      }();

      return std::format("{:.1f} kHz · {}-bit · {}", format.sampleRate / kKhzMultiplier, format.bitDepth, channelsText);
    }

    std::string computePipelineTooltip(rt::PlaybackState const& state)
    {
      auto tooltip = std::string{"Audio Pipeline:\n"};
      auto const nodeTypeString = [](audio::flow::NodeType type)
      {
        using Type = audio::flow::NodeType;

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

      auto currentId = std::string{"ao-decoder"};
      auto visited = std::unordered_set<std::string>{};

      while (!currentId.empty() && !visited.contains(currentId))
      {
        visited.insert(currentId);
        auto const it = std::ranges::find(state.flow.nodes, currentId, &audio::flow::Node::id);

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

      if (!state.qualityTooltip.empty())
      {
        std::format_to(std::back_inserter(tooltip), "\n{}", state.qualityTooltip);
      }

      return tooltip;
    }

    AudioQualityCategory mapQuality(audio::Quality quality)
    {
      using Quality = audio::Quality;

      switch (quality)
      {
        case Quality::BitwisePerfect:
        case Quality::LosslessPadded: return AudioQualityCategory::Perfect;
        case Quality::LosslessFloat: return AudioQualityCategory::Lossless;
        case Quality::LinearIntervention: return AudioQualityCategory::Intervention;
        case Quality::LossySource: return AudioQualityCategory::Lossy;
        case Quality::Clipped: return AudioQualityCategory::Clipped;
        case Quality::Unknown: return AudioQualityCategory::Unknown;
      }

      return AudioQualityCategory::Unknown;
    }
  } // namespace

  NowPlayingViewModel::NowPlayingViewModel(rt::PlaybackService& playback,
                                           std::function<void(NowPlayingViewState const&)> onRender)
    : _playback{playback}, _onRender{std::move(onRender)}
  {
    auto const refreshCallback = [this] { refresh(); };
    auto const refreshCallbackWithArg = [this](auto const&) { refresh(); };

    _startedSub = _playback.onStarted(refreshCallback);
    _pausedSub = _playback.onPaused(refreshCallback);
    _idleSub = _playback.onIdle(refreshCallback);
    _stoppedSub = _playback.onStopped(refreshCallback);
    _outputChangedSub = _playback.onOutputChanged(refreshCallbackWithArg);
    _qualityChangedSub = _playback.onQualityChanged(refreshCallbackWithArg);
    _nowPlayingSub = _playback.onNowPlayingChanged(refreshCallbackWithArg);

    refresh();
  }

  void NowPlayingViewModel::refresh()
  {
    auto const& state = _playback.state();
    auto view = NowPlayingViewState{};

    if (state.trackTitle.empty())
    {
      view.title = "Not Playing";
      view.streamInfo = state.ready ? "" : "Connecting to audio engine...";
      view.isActive = false;
    }
    else
    {
      view.title = state.trackTitle;
      view.artist = state.trackArtist.empty() ? "Unknown Artist" : state.trackArtist;

      if (!state.trackArtist.empty())
      {
        view.combinedStatus = std::format("{} - {}", state.trackArtist, state.trackTitle);
      }
      else
      {
        view.combinedStatus = state.trackTitle;
      }

      auto const it = std::ranges::find_if(state.flow.nodes,
                                           [](auto const& node)
                                           { return node.type == audio::flow::NodeType::Decoder && node.optFormat; });

      view.streamInfo = (it != state.flow.nodes.end() && it->optFormat) ? formatStream(*it->optFormat) : std::string{};
      view.pipelineTooltip = computePipelineTooltip(state);
      view.isActive = (state.quality != audio::Quality::Unknown);
      view.qualityCategory = mapQuality(state.quality);
    }

    if (_onRender)
    {
      _onRender(view);
    }
  }

  NowPlayingActionCommand NowPlayingViewModel::resolveAction(NowPlayingFieldAction action, rt::TrackField field) const
  {
    auto const& state = _playback.state();
    auto cmd = NowPlayingActionCommand{};

    switch (action)
    {
      case NowPlayingFieldAction::Reveal: cmd.type = NowPlayingActionCommand::Type::Reveal; break;

      case NowPlayingFieldAction::PlayPause: cmd.type = NowPlayingActionCommand::Type::PlayPause; break;

      case NowPlayingFieldAction::FilterByField:
      {
        auto value = std::string{};

        switch (field)
        {
          case rt::TrackField::Title: value = state.trackTitle; break;
          case rt::TrackField::Artist: value = state.trackArtist; break;
          default: break;
        }

        if (auto const variable = rt::trackFieldFilterExpressionVariable(field); !variable.empty() && !value.empty())
        {
          cmd.type = NowPlayingActionCommand::Type::Navigate;
          cmd.navigateQuery = std::format("{} = {}", variable, quoteExpressionString(value));
        }

        break;
      }

      case NowPlayingFieldAction::None:
      default: cmd.type = NowPlayingActionCommand::Type::None; break;
    }

    return cmd;
  }

  std::string NowPlayingViewModel::getFieldText(NowPlayingViewState const& view, rt::TrackField field)
  {
    switch (field)
    {
      case rt::TrackField::Title: return view.title;
      case rt::TrackField::Artist: return view.artist;
      default: return "";
    }
  }
} // namespace ao::uimodel::playback
