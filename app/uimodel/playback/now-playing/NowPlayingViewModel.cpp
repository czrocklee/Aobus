// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Device.h>
#include <ao/audio/Quality.h>
#include <ao/audio/Transport.h>
#include <ao/audio/flow/Graph.h>
#include <ao/query/Expression.h>
#include <ao/query/Serializer.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/playback/now-playing/NowPlayingViewModel.h>
#include <ao/uimodel/playback/quality/AudioQualityFormatter.h>

#include <algorithm>
#include <format>
#include <functional>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  namespace
  {
    std::string quoteExpressionString(std::string_view value)
    {
      return query::serialize(query::ConstantExpression{std::string{value}});
    }

    std::string sourceStreamInfo(rt::QualityState const& quality)
    {
      auto const it =
        std::ranges::find_if(quality.assessments,
                             [](auto const& assessment)
                             { return assessment.nodeType == audio::flow::NodeType::Source && assessment.optFormat; });

      // The source node carries the track's native format, so show its true
      // resolution (valid bits) rather than a padded transport container width.
      return (it != quality.assessments.end() && it->optFormat) ? audioFormatLabel(*it->optFormat, true)
                                                                : std::string{};
    }
  } // namespace

  NowPlayingViewModel::NowPlayingViewModel(rt::PlaybackService& playback,
                                           std::function<void(NowPlayingViewState const&)> onRender)
    : _playback{playback}, _onRender{std::move(onRender)}
  {
    auto const refreshCallback = [this] { refresh(); };
    auto const refreshIgnoringPayload = [this](auto const&) { refresh(); };

    _startedSub = _playback.onStarted(refreshCallback);
    _pausedSub = _playback.onPaused(refreshCallback);
    _idleSub = _playback.onIdle(refreshCallback);
    _stoppedSub = _playback.onStopped(refreshCallback);
    _outputDeviceChangedSub = _playback.onOutputDeviceChanged(refreshIgnoringPayload);
    _qualityChangedSub = _playback.onQualityChanged(refreshIgnoringPayload);
    _nowPlayingSub = _playback.onNowPlayingChanged(refreshIgnoringPayload);

    refresh();
  }

  NowPlayingActionCommand::Type resolveNowPlayingPlayPauseCommand(audio::Transport const transport) noexcept
  {
    return transport == audio::Transport::Playing ? NowPlayingActionCommand::Type::Pause
                                                  : NowPlayingActionCommand::Type::Resume;
  }

  void NowPlayingViewModel::refresh()
  {
    auto const& state = _playback.state();
    auto view = NowPlayingViewState{};

    if (state.nowPlaying.title.empty())
    {
      view.title = "Not Playing";
      view.streamInfo = state.ready ? "" : "Connecting to audio engine...";
      view.isActive = false;
    }
    else
    {
      view.title = state.nowPlaying.title;
      view.artist = state.nowPlaying.artist.empty() ? "Unknown Artist" : state.nowPlaying.artist;

      if (!state.nowPlaying.artist.empty())
      {
        view.combinedStatus = std::format("{} - {}", state.nowPlaying.artist, state.nowPlaying.title);
      }
      else
      {
        view.combinedStatus = state.nowPlaying.title;
      }

      auto const presentation = audioQualityPresentation(state.quality);
      view.streamInfo = sourceStreamInfo(state.quality);

      auto plainTextFallback = std::string{"Audio Pipeline:\n"};

      if (!presentation.headline.empty())
      {
        std::format_to(std::back_inserter(plainTextFallback), "\n{}", presentation.headline);
      }

      auto deviceName = std::string{};
      auto deviceIconName = std::string{};

      for (auto const& backend : state.output.availableBackends)
      {
        for (auto const& device : backend.devices)
        {
          if (device.id == state.output.selectedDevice.deviceId)
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

      view.audioPipeline = AudioPipelineView{.quality = state.quality,
                                             .deviceName = std::move(deviceName),
                                             .deviceIconName = std::move(deviceIconName),
                                             .plainTextFallback = plainTextFallback};

      view.isActive = (state.quality.overall != audio::Quality::Unknown);
      view.qualityCategory = presentation.category;
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

      case NowPlayingFieldAction::PlayPause: cmd.type = resolveNowPlayingPlayPauseCommand(state.transport); break;

      case NowPlayingFieldAction::FilterByField:
      {
        auto value = std::string{};

        switch (field)
        {
          case rt::TrackField::Title: value = state.nowPlaying.title; break;
          case rt::TrackField::Artist: value = state.nowPlaying.artist; break;
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
} // namespace ao::uimodel
