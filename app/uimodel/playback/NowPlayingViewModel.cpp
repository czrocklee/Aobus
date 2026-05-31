// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Backend.h>
#include <ao/audio/flow/Graph.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/playback/AudioQualityFormat.h>
#include <ao/uimodel/playback/NowPlayingViewModel.h>

#include <algorithm>
#include <format>
#include <functional>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
  } // namespace

  namespace
  {
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

      view.streamInfo =
        (it != state.flow.nodes.end() && it->optFormat) ? audioFormatLabel(*it->optFormat) : std::string{};

      auto plainTextFallback = std::string{"Audio Pipeline:\n"};
      auto const conclusionText = audioQualityConclusion(state.quality);

      if (!conclusionText.empty())
      {
        std::format_to(std::back_inserter(plainTextFallback), "\n{}", conclusionText);
      }

      auto deviceName = std::string{};
      auto deviceIconName = std::string{};

      for (auto const& backend : state.availableOutputs)
      {
        for (auto const& device : backend.devices)
        {
          if (device.id == state.selectedOutput.deviceId)
          {
            deviceName = device.displayName;
            deviceIconName = backend.iconName;
            break;
          }
        }

        if (!deviceName.empty())
        {
          break;
        }
      }

      view.audioPipeline = AudioPipelineView{.flow = state.flow,
                                             .quality = state.quality,
                                             .assessments = state.qualityAssessments,
                                             .deviceName = std::move(deviceName),
                                             .deviceIconName = std::move(deviceIconName),
                                             .plainTextFallback = plainTextFallback};

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

  std::string NowPlayingViewModel::fieldText(NowPlayingViewState const& view, rt::TrackField field)
  {
    switch (field)
    {
      case rt::TrackField::Title: return view.title;
      case rt::TrackField::Artist: return view.artist;
      default: return "";
    }
  }
} // namespace ao::uimodel::playback
