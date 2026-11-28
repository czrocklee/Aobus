// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/playback/transport/TransportViewModel.h>

#include <ao/audio/Transport.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/playback/command/PlaybackActions.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/command/PlaybackCommandText.h>

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

    bool isPresentedAsPlaying(audio::Transport const transport) noexcept
    {
      return transport == audio::Transport::Opening || transport == audio::Transport::Buffering ||
             transport == audio::Transport::Playing || transport == audio::Transport::Seeking;
    }

    TransportViewState describeTransportButton(PlaybackCommand command,
                                               i18n::MessageCatalog const& textCatalog,
                                               rt::PlaybackTransportSnapshot const& transport,
                                               rt::PlaybackSuccessionSnapshot const& succession,
                                               bool enabled,
                                               bool showLabel)
    {
      auto view = TransportViewState{};
      view.playing = command == PlaybackCommand::PlayPause && isPresentedAsPlaying(transport.transport);
      auto const presentedCommand = view.playing ? PlaybackCommand::Pause : command;
      view.icon = iconForCommand(presentedCommand);
      view.tooltip = transportControlLabel(textCatalog, presentedCommand);
      view.enabled = enabled;

      if (showLabel)
      {
        view.label = view.tooltip;
      }

      if (command == PlaybackCommand::ToggleShuffle)
      {
        view.engaged = succession.shuffle == rt::ShuffleMode::On;
      }
      else if (command == PlaybackCommand::CycleRepeat)
      {
        auto const repeatOne = succession.repeat == rt::RepeatMode::One;
        view.icon = repeatOne ? TransportIcon::RepeatOne : TransportIcon::Repeat;
        view.engaged = repeatOne || succession.repeat == rt::RepeatMode::All;
      }

      return view;
    }
  } // namespace

  TransportViewModel::TransportViewModel(rt::PlaybackService& playback,
                                         PlaybackActions& actions,
                                         i18n::MessageCatalog textCatalog,
                                         PlaybackCommand command,
                                         bool showLabel,
                                         std::function<void(TransportViewState const&)> onRender)
    : _playback{playback}
    , _actions{actions}
    , _textCatalog{std::move(textCatalog)}
    , _command{command}
    , _showLabel{showLabel}
    , _onRender{std::move(onRender)}
  {
    _availabilitySub = _actions.onAvailabilityChanged(_command, [this] { refresh(); });
    refresh();
  }

  void TransportViewModel::handleClick()
  {
    _actions.tryExecute(_command);
  }

  void TransportViewModel::refresh()
  {
    auto const& snapshot = _playback.snapshot();
    auto const view = describeTransportButton(
      _command, _textCatalog, snapshot.transport, snapshot.succession, _actions.isEnabled(_command), _showLabel);

    if (_onRender)
    {
      _onRender(view);
    }
  }
} // namespace ao::uimodel
