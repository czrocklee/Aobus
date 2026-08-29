// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/playback/now-playing/NowPlayingViewModel.h>

#include <ao/audio/Device.h>
#include <ao/audio/Quality.h>
#include <ao/audio/Transport.h>
#include <ao/audio/flow/Graph.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/query/Expression.h>
#include <ao/query/Serializer.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/playback/output/PlaybackOutputText.h>
#include <ao/uimodel/playback/quality/AudioQualityFormatter.h>
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>

#include <algorithm>
#include <array>
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

    std::string sourceStreamInfo(AudioQualityFormatter const& formatter, rt::QualityState const& quality)
    {
      auto const it =
        std::ranges::find_if(quality.assessments,
                             [](auto const& assessment)
                             { return assessment.nodeType == audio::flow::NodeType::Source && assessment.optFormat; });

      // The source node carries the track's native format, so show its true
      // resolution (valid bits) rather than a padded transport container width.
      return (it != quality.assessments.end() && it->optFormat) ? formatter.formatLabel(*it->optFormat) : std::string{};
    }

    struct SelectedDevicePresentation final
    {
      std::string name{};
      AudioIconKind iconKind = AudioIconKind::OutputDevice;
    };

    SelectedDevicePresentation resolveSelectedDevicePresentation(i18n::MessageCatalog const& textCatalog,
                                                                 rt::OutputState const& output)
    {
      for (auto const& backend : output.availableBackends)
      {
        if (backend.id != output.selectedDevice.backendId)
        {
          continue;
        }

        for (auto const& device : backend.devices)
        {
          if (device.id != output.selectedDevice.deviceId)
          {
            continue;
          }

          auto name = device.displayName;

          if (device.isDefault && device.id.empty() && name.empty())
          {
            name = i18n::requiredText(textCatalog, i18n::MessageId::SystemDefaultOutputDevice);
          }

          return {.name = std::move(name), .iconKind = audioBackendPresentation(textCatalog, backend.id).iconKind};
        }
      }

      return {};
    }

    NowPlayingActionCommand::Type playPauseCommand(audio::Transport const transport) noexcept
    {
      return transport == audio::Transport::Playing ? NowPlayingActionCommand::Type::Pause
                                                    : NowPlayingActionCommand::Type::Resume;
    }
  } // namespace

  NowPlayingViewModel::NowPlayingViewModel(rt::PlaybackService& playback,
                                           i18n::MessageCatalog textCatalog,
                                           std::function<void(NowPlayingViewState const&)> onRender)
    : _playback{playback}, _onRender{std::move(onRender)}, _textCatalog{std::move(textCatalog)}
  {
    auto const& transport = _playback.snapshot().transport;
    _lastNowPlaying = transport.nowPlaying;
    _lastOutput = transport.output;
    _lastQuality = transport.quality;
    _lastReady = transport.ready;
    _snapshotSub =
      _playback.events().onSnapshot([this](rt::PlaybackSnapshot const& snapshot) { handleSnapshot(snapshot); });
    refresh();
  }

  void NowPlayingViewModel::refresh()
  {
    render(_playback.snapshot().transport);
  }

  void NowPlayingViewModel::handleSnapshot(rt::PlaybackSnapshot const& snapshot)
  {
    auto const& transport = snapshot.transport;

    if (transport.nowPlaying == _lastNowPlaying && transport.output == _lastOutput &&
        transport.quality == _lastQuality && transport.ready == _lastReady)
    {
      return;
    }

    _lastNowPlaying = transport.nowPlaying;
    _lastOutput = transport.output;
    _lastQuality = transport.quality;
    _lastReady = transport.ready;
    render(transport);
  }

  void NowPlayingViewModel::render(rt::PlaybackTransportSnapshot const& state)
  {
    auto view = NowPlayingViewState{};
    view.coverArtId = state.nowPlaying.coverArtId;
    auto const placeholderCandidates = std::array<std::string_view, 3>{
      state.nowPlaying.album,
      state.nowPlaying.artist,
      state.nowPlaying.title,
    };
    view.coverArtPlaceholderIdentity = makeCoverArtPlaceholderIdentity(placeholderCandidates);

    if (state.nowPlaying.title.empty())
    {
      view.title = i18n::requiredText(_textCatalog, i18n::MessageId::PlaybackNotPlaying);
      view.streamInfo =
        state.ready ? ""
                    : std::string{i18n::requiredText(_textCatalog, i18n::MessageId::PlaybackConnectingAudioEngine)};
      view.isActive = false;
    }
    else
    {
      view.title = state.nowPlaying.title;
      view.artist = state.nowPlaying.artist.empty()
                      ? std::string{i18n::requiredText(_textCatalog, i18n::MessageId::PlaybackUnknownArtist)}
                      : state.nowPlaying.artist;

      if (!state.nowPlaying.artist.empty())
      {
        view.combinedStatus = std::format("{} - {}", state.nowPlaying.artist, state.nowPlaying.title);
      }
      else
      {
        view.combinedStatus = state.nowPlaying.title;
      }

      auto const qualityFormatter = AudioQualityFormatter{_textCatalog};
      auto const presentation = qualityFormatter.presentation(state.quality);
      view.streamInfo = sourceStreamInfo(qualityFormatter, state.quality);

      auto plainTextFallback = std::string{i18n::requiredText(_textCatalog, i18n::MessageId::PlaybackAudioPipeline)};
      plainTextFallback.append(":\n");

      if (!presentation.headline.empty())
      {
        std::format_to(std::back_inserter(plainTextFallback), "\n{}", presentation.headline);
      }

      auto devicePresentation = resolveSelectedDevicePresentation(_textCatalog, state.output);

      view.audioPipeline = AudioPipelineViewState{.quality = state.quality,
                                                  .deviceName = std::move(devicePresentation.name),
                                                  .deviceIconKind = devicePresentation.iconKind,
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
    auto const& snapshot = _playback.snapshot();
    auto const& state = snapshot.transport;
    auto cmd = NowPlayingActionCommand{};

    switch (action)
    {
      case NowPlayingFieldAction::Reveal: cmd.type = NowPlayingActionCommand::Type::Reveal; break;

      case NowPlayingFieldAction::PlayPause: cmd.type = playPauseCommand(state.transport); break;

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
