// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/CorePrimitives.h>
#include <ao/rt/PlaybackService.h>

#include <cstdint>
#include <functional>
#include <string>

namespace ao::uimodel
{
  class PlaybackQueueModel;

  enum class TransportAction : std::uint8_t
  {
    Play,
    Pause,
    Stop,
    PlayPause,
    Next,
    Previous,
    Shuffle,
    Repeat,
  };

  enum class TransportIcon : std::uint8_t
  {
    None,
    Play,
    Pause,
    Stop,
    Next,
    Previous,
    Shuffle,
    Repeat,
    RepeatOne,
  };

  struct TransportViewState final
  {
    TransportIcon icon = TransportIcon::None;
    std::string tooltip;
    std::string label;
    bool enabled = false;
    bool engaged = false;
    bool playing = false;
  };

  enum class TransportCommand : std::uint8_t
  {
    None,
    PlaySelection,
    Pause,
    Resume,
    Stop,
    Next,
    Previous,
    ToggleShuffle,
    CycleRepeat,
  };

  class TransportViewModel final
  {
  public:
    TransportViewModel(rt::PlaybackService& playback,
                       PlaybackQueueModel* queue,
                       TransportAction action,
                       std::function<void()> onPlaySelection,
                       bool showLabel,
                       std::function<void(TransportViewState const&)> onRender);

    TransportViewModel(TransportViewModel const&) = delete;
    TransportViewModel& operator=(TransportViewModel const&) = delete;
    TransportViewModel(TransportViewModel&&) = delete;
    TransportViewModel& operator=(TransportViewModel&&) = delete;

    ~TransportViewModel() = default;

    void handleClick();
    void refresh();

  private:
    rt::PlaybackService& _playback;
    PlaybackQueueModel* _queue;
    TransportAction _action;
    std::function<void()> _onPlaySelection;
    bool _showLabel;
    std::function<void(TransportViewState const&)> _onRender;

    rt::Subscription _startedSub;
    rt::Subscription _pausedSub;
    rt::Subscription _idleSub;
    rt::Subscription _stoppedSub;
    rt::Subscription _preparingSub;
    rt::Subscription _shuffleSub;
    rt::Subscription _repeatSub;
  };
} // namespace ao::uimodel
