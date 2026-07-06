// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Transport.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/command/PlaybackCommandSurface.h>
#include <ao/uimodel/playback/transport/TransportViewModel.h>

#include <functional>
#include <utility>

namespace ao::uimodel
{
  namespace
  {
    TransportIcon iconForAction(TransportAction action)
    {
      switch (action)
      {
        case TransportAction::Play: return TransportIcon::Play;
        case TransportAction::Pause: return TransportIcon::Pause;
        case TransportAction::Stop: return TransportIcon::Stop;
        case TransportAction::PlayPause: return TransportIcon::Play;
        case TransportAction::Next: return TransportIcon::Next;
        case TransportAction::Previous: return TransportIcon::Previous;
        case TransportAction::Shuffle: return TransportIcon::Shuffle;
        case TransportAction::Repeat: return TransportIcon::Repeat;
      }

      return TransportIcon::None;
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

    bool presentsAsPlaying(audio::Transport const transport) noexcept
    {
      return transport == audio::Transport::Opening || transport == audio::Transport::Buffering ||
             transport == audio::Transport::Playing || transport == audio::Transport::Seeking;
    }

    TransportViewState describeTransportButton(TransportAction action,
                                               rt::PlaybackState const& state,
                                               bool enabled,
                                               bool showLabel)
    {
      auto view = TransportViewState{};
      bool const isPlaying = presentsAsPlaying(state.transport);

      view.icon = iconForAction(action);
      view.tooltip = labelForAction(action);
      view.enabled = enabled;

      if (showLabel)
      {
        view.label = labelForAction(action);
      }

      if (action == TransportAction::PlayPause)
      {
        view.icon = isPlaying ? TransportIcon::Pause : TransportIcon::Play;
        view.tooltip = isPlaying ? "Pause" : "Play";
        view.playing = isPlaying;

        if (showLabel)
        {
          view.label = isPlaying ? "Pause" : "Play";
        }
      }
      else if (action == TransportAction::Shuffle)
      {
        view.engaged = (state.mode.shuffle == rt::ShuffleMode::On);
      }
      else if (action == TransportAction::Repeat)
      {
        if (state.mode.repeat == rt::RepeatMode::All)
        {
          view.icon = TransportIcon::Repeat;
          view.engaged = true;
        }
        else if (state.mode.repeat == rt::RepeatMode::One)
        {
          view.icon = TransportIcon::RepeatOne;
          view.engaged = true;
        }
        else
        {
          view.icon = TransportIcon::Repeat;
          view.engaged = false;
        }
      }

      return view;
    }

    PlaybackCommand commandForAction(TransportAction action) noexcept
    {
      switch (action)
      {
        case TransportAction::Play: return PlaybackCommand::Play;
        case TransportAction::Pause: return PlaybackCommand::Pause;
        case TransportAction::Stop: return PlaybackCommand::Stop;
        case TransportAction::PlayPause: return PlaybackCommand::PlayPause;
        case TransportAction::Next: return PlaybackCommand::Next;
        case TransportAction::Previous: return PlaybackCommand::Previous;
        case TransportAction::Shuffle: return PlaybackCommand::ToggleShuffle;
        case TransportAction::Repeat: return PlaybackCommand::CycleRepeat;
      }

      return PlaybackCommand::PlayPause;
    }
  } // namespace

  TransportViewModel::TransportViewModel(rt::PlaybackService& playback,
                                         PlaybackCommandSurface& commands,
                                         TransportAction action,
                                         bool showLabel,
                                         std::function<void(TransportViewState const&)> onRender)
    : _playback{playback}, _commands{commands}, _action{action}, _showLabel{showLabel}, _onRender{std::move(onRender)}
  {
    _availabilitySub = _commands.onAvailabilityChanged(commandForAction(_action), [this] { refresh(); });
    refresh();
  }

  void TransportViewModel::handleClick()
  {
    _commands.execute(commandForAction(_action));
  }

  void TransportViewModel::refresh()
  {
    auto const command = commandForAction(_action);
    auto const view = describeTransportButton(_action, _playback.state(), _commands.enabled(command), _showLabel);

    if (_onRender)
    {
      _onRender(view);
    }
  }
} // namespace ao::uimodel
