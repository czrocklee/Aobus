// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/async/Subscription.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>

#include <cstdint>
#include <functional>
#include <string>

namespace ao::rt
{
  class PlaybackService;
}

namespace ao::uimodel
{
  class PlaybackCommandSurface;

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

  class TransportViewModel final
  {
  public:
    TransportViewModel(rt::PlaybackService& playback,
                       PlaybackCommandSurface& commands,
                       PlaybackCommand command,
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
    PlaybackCommandSurface& _commands;
    PlaybackCommand _command;
    bool _showLabel;
    std::function<void(TransportViewState const&)> _onRender;

    async::Subscription _availabilitySub;
  };
} // namespace ao::uimodel
