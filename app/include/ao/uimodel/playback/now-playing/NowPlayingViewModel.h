// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/async/Subscription.h>
#include <ao/audio/Transport.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/playback/quality/AudioQualityFormatter.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include <cstdint>
#include <functional>
#include <string>

namespace ao::rt
{
  class PlaybackService;
}

namespace ao::uimodel
{
  namespace detail
  {
    std::string formatPipelineTooltip(rt::PlaybackState const& state);
  }

  enum class NowPlayingFieldAction : std::uint8_t
  {
    None,
    Reveal,
    PlayPause,
    FilterByField
  };

  struct NowPlayingActionCommand final
  {
    enum class Type : std::uint8_t
    {
      None,
      Reveal,
      Pause,
      Resume,
      Navigate
    };

    Type type = Type::None;
    std::string navigateQuery;
  };

  NowPlayingActionCommand::Type resolveNowPlayingPlayPauseCommand(audio::Transport transport) noexcept;

  struct AudioPipelineViewState final
  {
    rt::QualityState quality{};
    std::string deviceName{};
    AudioIconKind deviceIconKind = AudioIconKind::OutputDevice;
    std::string plainTextFallback{};
  };

  struct NowPlayingViewState final
  {
    std::string streamInfo;
    AudioPipelineViewState audioPipeline{};
    AudioQualityCategory qualityCategory = AudioQualityCategory::Unknown;
    bool isActive = false;

    std::string title;
    std::string artist;
    std::string combinedStatus;
  };

  class NowPlayingViewModel final
  {
  public:
    NowPlayingViewModel(rt::PlaybackService& playback, std::function<void(NowPlayingViewState const&)> onRender);

    NowPlayingViewModel(NowPlayingViewModel const&) = delete;
    NowPlayingViewModel& operator=(NowPlayingViewModel const&) = delete;
    NowPlayingViewModel(NowPlayingViewModel&&) = delete;
    NowPlayingViewModel& operator=(NowPlayingViewModel&&) = delete;

    ~NowPlayingViewModel() = default;

    NowPlayingActionCommand resolveAction(NowPlayingFieldAction action, rt::TrackField field) const;

    static std::string fieldText(NowPlayingViewState const& view, rt::TrackField field);

  private:
    void refresh();

    rt::PlaybackService& _playback;
    std::function<void(NowPlayingViewState const&)> _onRender;
    PresentationTextCatalog _textCatalog;

    async::Subscription _snapshotSub;
  };
} // namespace ao::uimodel
