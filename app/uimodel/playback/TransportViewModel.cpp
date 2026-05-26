// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ao/rt/StateTypes.h"
#include <ao/audio/Types.h>
#include <ao/uimodel/playback/PlaybackQueueModel.h>
#include <ao/uimodel/playback/TransportViewModel.h>

#include <functional>
#include <utility>

namespace ao::uimodel::playback
{
  namespace
  {
    char const* iconForAction(TransportAction action)
    {
      switch (action)
      {
        case TransportAction::Play: return "media-playback-start-symbolic";
        case TransportAction::Pause: return "media-playback-pause-symbolic";
        case TransportAction::Stop: return "media-playback-stop-symbolic";
        case TransportAction::PlayPause: return "media-playback-start-symbolic";
        case TransportAction::Next: return "media-skip-forward-symbolic";
        case TransportAction::Previous: return "media-skip-backward-symbolic";
        case TransportAction::Shuffle: return "media-playlist-shuffle-symbolic";
        case TransportAction::Repeat: return "media-playlist-repeat-symbolic";
      }

      return "media-playback-start-symbolic";
    }

    char const* labelForAction(TransportAction action)
    {
      switch (action)
      {
        case TransportAction::Play: return "Play";
        case TransportAction::Pause: return "Pause";
        case TransportAction::Stop: return "Stop";
        case TransportAction::PlayPause: return "Play";
        case TransportAction::Next: return "Next Track";
        case TransportAction::Previous: return "Previous Track";
        case TransportAction::Shuffle: return "Shuffle";
        case TransportAction::Repeat: return "Repeat";
      }

      return "";
    }

    TransportViewState describeTransportButton(TransportAction action,
                                               rt::PlaybackState const& state,
                                               bool sequenceActive,
                                               bool showLabel)
    {
      auto view = TransportViewState{};
      bool const isPlaying = (state.transport == audio::Transport::Playing);

      view.transportGlyph = iconForAction(action);
      view.tooltip = labelForAction(action);

      if (showLabel)
      {
        view.label = labelForAction(action);
      }

      switch (action)
      {
        case TransportAction::Play: view.enabled = (state.ready && !isPlaying); break;

        case TransportAction::Pause: view.enabled = (state.ready && isPlaying); break;

        case TransportAction::Stop: view.enabled = (state.transport != audio::Transport::Idle); break;

        case TransportAction::PlayPause:
          view.transportGlyph = isPlaying ? "media-playback-pause-symbolic" : "media-playback-start-symbolic";
          view.tooltip = isPlaying ? "Pause" : "Play";
          view.playing = isPlaying;

          if (showLabel)
          {
            view.label = isPlaying ? "Pause" : "Play";
          }

          view.enabled = state.ready;
          break;

        case TransportAction::Next:
        case TransportAction::Previous: view.enabled = (state.ready && sequenceActive); break;

        case TransportAction::Shuffle:
          view.engaged = (state.shuffleMode == rt::ShuffleMode::On);
          view.enabled = state.ready;
          break;

        case TransportAction::Repeat:
          if (state.repeatMode == rt::RepeatMode::All)
          {
            view.transportGlyph = "media-playlist-repeat-symbolic";
            view.engaged = true;
          }
          else if (state.repeatMode == rt::RepeatMode::One)
          {
            view.transportGlyph = "media-playlist-repeat-song-symbolic";
            view.engaged = true;
          }
          else
          {
            view.transportGlyph = "media-playlist-repeat-symbolic";
            view.engaged = false;
          }

          view.enabled = state.ready;
          break;
      }

      return view;
    }

    TransportCommand resolveTransportButtonClick(TransportAction action, rt::PlaybackState const& state)
    {
      switch (action)
      {
        case TransportAction::Play:
          if (state.transport == audio::Transport::Paused)
          {
            return TransportCommand::Resume;
          }

          if (state.transport != audio::Transport::Playing)
          {
            return TransportCommand::PlaySelection;
          }

          return TransportCommand::None;

        case TransportAction::Pause: return TransportCommand::Pause;

        case TransportAction::Stop: return TransportCommand::Stop;

        case TransportAction::PlayPause:
          if (state.transport == audio::Transport::Paused)
          {
            return TransportCommand::Resume;
          }

          if (state.transport == audio::Transport::Playing)
          {
            return TransportCommand::Pause;
          }

          return TransportCommand::PlaySelection;

        case TransportAction::Next: return TransportCommand::Next;

        case TransportAction::Previous: return TransportCommand::Previous;

        case TransportAction::Shuffle: return TransportCommand::ToggleShuffle;

        case TransportAction::Repeat: return TransportCommand::CycleRepeat;
      }

      return TransportCommand::None;
    }
  } // namespace

  TransportViewModel::TransportViewModel(rt::PlaybackService& playback,
                                         PlaybackQueueModel* queue,
                                         TransportAction action,
                                         std::function<void()> onPlaySelection,
                                         bool showLabel,
                                         std::function<void(TransportViewState const&)> onRender)
    : _playback{playback}
    , _queue{queue}
    , _action{action}
    , _onPlaySelection{std::move(onPlaySelection)}
    , _showLabel{showLabel}
    , _onRender{std::move(onRender)}
  {
    auto const refreshCallback = [this] { refresh(); };
    _startedSub = _playback.onStarted(refreshCallback);
    _pausedSub = _playback.onPaused(refreshCallback);
    _idleSub = _playback.onIdle(refreshCallback);
    _stoppedSub = _playback.onStopped(refreshCallback);

    if (_action == TransportAction::PlayPause)
    {
      _preparingSub = _playback.onPreparing(refreshCallback);
    }

    if (_action == TransportAction::Shuffle)
    {
      _shuffleSub = _playback.onShuffleModeChanged([this](auto const&) { refresh(); });
    }

    if (_action == TransportAction::Repeat)
    {
      _repeatSub = _playback.onRepeatModeChanged([this](auto const&) { refresh(); });
    }

    refresh();
  }

  void TransportViewModel::handleClick()
  {
    auto const command = resolveTransportButtonClick(_action, _playback.state());

    switch (command)
    {
      case TransportCommand::None: break;

      case TransportCommand::PlaySelection:
        if (_onPlaySelection)
        {
          _onPlaySelection();
        }

        break;

      case TransportCommand::Pause: _playback.pause(); break;
      case TransportCommand::Resume: _playback.resume(); break;
      case TransportCommand::Stop: _playback.stop(); break;

      case TransportCommand::Next:
        if (_queue != nullptr)
        {
          _queue->next();
        }

        break;

      case TransportCommand::Previous:
        if (_queue != nullptr)
        {
          _queue->previous();
        }

        break;

      case TransportCommand::ToggleShuffle:
        if (_queue != nullptr)
        {
          _queue->setShuffleMode(_playback.state().shuffleMode == rt::ShuffleMode::Off ? rt::ShuffleMode::On
                                                                                       : rt::ShuffleMode::Off);
        }

        break;

      case TransportCommand::CycleRepeat:
        if (_queue != nullptr)
        {
          if (auto const mode = _playback.state().repeatMode; mode == rt::RepeatMode::Off)
          {
            _queue->setRepeatMode(rt::RepeatMode::All);
          }
          else if (mode == rt::RepeatMode::All)
          {
            _queue->setRepeatMode(rt::RepeatMode::One);
          }
          else
          {
            _queue->setRepeatMode(rt::RepeatMode::Off);
          }
        }

        break;
    }
  }

  void TransportViewModel::refresh()
  {
    bool const sequenceActive = (_queue != nullptr && _queue->isActive());
    auto const view = describeTransportButton(_action, _playback.state(), sequenceActive, _showLabel);

    if (_onRender)
    {
      _onRender(view);
    }
  }
} // namespace ao::uimodel::playback
