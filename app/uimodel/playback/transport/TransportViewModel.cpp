// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Transport.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/command/PlaybackCommandSurface.h>
#include <ao/uimodel/playback/transport/TransportViewModel.h>

#include <functional>
#include <utility>

namespace ao::uimodel
{
  namespace
  {
    TransportIcon iconForCommand(PlaybackCommand command)
    {
      switch (command)
      {
        case PlaybackCommand::Play: return TransportIcon::Play;
        case PlaybackCommand::Pause: return TransportIcon::Pause;
        case PlaybackCommand::Stop: return TransportIcon::Stop;
        case PlaybackCommand::PlayPause: return TransportIcon::Play;
        case PlaybackCommand::Next: return TransportIcon::Next;
        case PlaybackCommand::Previous: return TransportIcon::Previous;
        case PlaybackCommand::ToggleShuffle: return TransportIcon::Shuffle;
        case PlaybackCommand::CycleRepeat: return TransportIcon::Repeat;
      }

      return TransportIcon::None;
    }

    char const* labelForCommand(PlaybackCommand command)
    {
      switch (command)
      {
        case PlaybackCommand::Play: return "Play";
        case PlaybackCommand::Pause: return "Pause";
        case PlaybackCommand::Stop: return "Stop";
        case PlaybackCommand::PlayPause: return "Play";
        case PlaybackCommand::Next: return "Next Track";
        case PlaybackCommand::Previous: return "Previous Track";
        case PlaybackCommand::ToggleShuffle: return "Shuffle";
        case PlaybackCommand::CycleRepeat: return "Repeat";
      }

      return "";
    }

    bool isPresentedAsPlaying(audio::Transport const transport) noexcept
    {
      return transport == audio::Transport::Opening || transport == audio::Transport::Buffering ||
             transport == audio::Transport::Playing || transport == audio::Transport::Seeking;
    }

    TransportViewState describeTransportButton(PlaybackCommand command,
                                               rt::PlaybackTransportSnapshot const& transport,
                                               rt::PlaybackSuccessionSnapshot const& succession,
                                               bool enabled,
                                               bool showLabel)
    {
      auto view = TransportViewState{};
      bool const isPlaying = isPresentedAsPlaying(transport.transport);

      view.icon = iconForCommand(command);
      view.tooltip = labelForCommand(command);
      view.enabled = enabled;

      if (showLabel)
      {
        view.label = labelForCommand(command);
      }

      if (command == PlaybackCommand::PlayPause)
      {
        view.icon = isPlaying ? TransportIcon::Pause : TransportIcon::Play;
        view.tooltip = isPlaying ? "Pause" : "Play";
        view.playing = isPlaying;

        if (showLabel)
        {
          view.label = isPlaying ? "Pause" : "Play";
        }
      }
      else if (command == PlaybackCommand::ToggleShuffle)
      {
        view.engaged = (succession.shuffle == rt::ShuffleMode::On);
      }
      else if (command == PlaybackCommand::CycleRepeat)
      {
        if (succession.repeat == rt::RepeatMode::All)
        {
          view.icon = TransportIcon::Repeat;
          view.engaged = true;
        }
        else if (succession.repeat == rt::RepeatMode::One)
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
  } // namespace

  TransportViewModel::TransportViewModel(rt::PlaybackService& playback,
                                         PlaybackCommandSurface& commands,
                                         PlaybackCommand command,
                                         bool showLabel,
                                         std::function<void(TransportViewState const&)> onRender)
    : _playback{playback}, _commands{commands}, _command{command}, _showLabel{showLabel}, _onRender{std::move(onRender)}
  {
    _availabilitySub = _commands.onAvailabilityChanged(_command, [this] { refresh(); });
    refresh();
  }

  void TransportViewModel::handleClick()
  {
    _commands.execute(_command);
  }

  void TransportViewModel::refresh()
  {
    auto const& snapshot = _playback.snapshot();
    auto const view = describeTransportButton(
      _command, snapshot.transport, snapshot.succession, _commands.isEnabled(_command), _showLabel);

    if (_onRender)
    {
      _onRender(view);
    }
  }
} // namespace ao::uimodel
