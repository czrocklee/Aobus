// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::uimodel
{
  enum class PlaybackCommand : std::uint8_t
  {
    Play,
    Pause,
    PlayPause,
    Stop,
    Next,
    Previous,
    ToggleShuffle,
    CycleRepeat,
  };

  /**
   * @brief The id a document or a keymap names @p command by.
   *
   * The same spelling every shell accepts, and the same one the `playback.*`
   * action ids carry after their category, so a reader who knows one knows the
   * other.
   */
  constexpr std::string_view playbackCommandId(PlaybackCommand const command) noexcept
  {
    switch (command)
    {
      case PlaybackCommand::Play: return "play";
      case PlaybackCommand::Pause: return "pause";
      case PlaybackCommand::PlayPause: return "playPause";
      case PlaybackCommand::Stop: return "stop";
      case PlaybackCommand::Next: return "next";
      case PlaybackCommand::Previous: return "previous";
      case PlaybackCommand::ToggleShuffle: return "toggleShuffle";
      case PlaybackCommand::CycleRepeat: return "cycleRepeat";
    }

    return {};
  }

  /// The command @p id names, or nullopt when nothing does.
  std::optional<PlaybackCommand> playbackCommandFor(std::string_view id) noexcept;

  /// Every command id, for a descriptor that offers the whole transport.
  std::vector<std::string> playbackCommandIds();

  /// Every command, for a shell registering the whole transport.
  std::span<PlaybackCommand const> playbackCommands() noexcept;

  /**
   * @brief The action id a shell registers @p command under.
   *
   * A transport command is a shell action in every frontend, and naming it is
   * the same act everywhere, so the id is derived here rather than spelled out
   * once per shell.
   */
  std::string playbackCommandActionId(PlaybackCommand command);

  /// The words a shell shows for @p command where actions are listed.
  constexpr std::string_view playbackCommandLabel(PlaybackCommand const command) noexcept
  {
    switch (command)
    {
      case PlaybackCommand::Play: return "Play";
      case PlaybackCommand::Pause: return "Pause";
      case PlaybackCommand::PlayPause: return "Play/Pause";
      case PlaybackCommand::Stop: return "Stop";
      case PlaybackCommand::Next: return "Next";
      case PlaybackCommand::Previous: return "Previous";
      case PlaybackCommand::ToggleShuffle: return "Toggle Shuffle";
      case PlaybackCommand::CycleRepeat: return "Cycle Repeat";
    }

    return {};
  }

  /// The category a transport action is listed under.
  inline constexpr auto kPlaybackActionCategory = std::string_view{"Playback"};
} // namespace ao::uimodel
