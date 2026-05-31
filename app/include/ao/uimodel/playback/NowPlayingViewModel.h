// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/Backend.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/flow/Graph.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackField.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ao::uimodel::playback
{
  namespace detail
  {
    std::string formatPipelineTooltip(rt::PlaybackState const& state);
  }

  enum class AudioQualityCategory : std::uint8_t
  {
    Unknown,
    Perfect,
    Lossless,
    Intervention,
    Lossy,
    Clipped,
  };

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
      PlayPause,
      Navigate
    };

    Type type = Type::None;
    std::string navigateQuery;
  };

  struct AudioPipelineView final
  {
    audio::flow::Graph flow{};
    audio::Quality quality = audio::Quality::Unknown;
    std::vector<audio::NodeQualityAssessment> assessments{};
    std::string deviceName{};
    std::string deviceIconName{};
    std::string plainTextFallback{};
  };

  struct NowPlayingViewState final
  {
    std::string streamInfo;
    AudioPipelineView audioPipeline{};
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

    rt::Subscription _startedSub;
    rt::Subscription _pausedSub;
    rt::Subscription _idleSub;
    rt::Subscription _stoppedSub;
    rt::Subscription _outputChangedSub;
    rt::Subscription _qualityChangedSub;
    rt::Subscription _nowPlayingSub;
  };
} // namespace ao::uimodel::playback
