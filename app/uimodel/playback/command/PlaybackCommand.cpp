// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/playback/command/PlaybackCommand.h>

#include <array>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::uimodel
{
  namespace
  {
    constexpr auto kCommands = std::to_array({PlaybackCommand::Play,
                                              PlaybackCommand::Pause,
                                              PlaybackCommand::PlayPause,
                                              PlaybackCommand::Stop,
                                              PlaybackCommand::Next,
                                              PlaybackCommand::Previous,
                                              PlaybackCommand::ToggleShuffle,
                                              PlaybackCommand::CycleRepeat});

    /// The action-id prefix every transport command shares.
    constexpr auto kPlaybackActionCategoryId = std::string_view{"playback"};
  } // namespace

  std::optional<PlaybackCommand> playbackCommandFor(std::string_view const id) noexcept
  {
    for (auto const command : kCommands)
    {
      if (playbackCommandId(command) == id)
      {
        return command;
      }
    }

    return std::nullopt;
  }

  std::span<PlaybackCommand const> playbackCommands() noexcept
  {
    return kCommands;
  }

  std::string playbackCommandActionId(PlaybackCommand const command)
  {
    return std::format("{}.{}", kPlaybackActionCategoryId, playbackCommandId(command));
  }

  std::vector<std::string> playbackCommandIds()
  {
    auto ids = std::vector<std::string>{};
    ids.reserve(kCommands.size());

    for (auto const command : kCommands)
    {
      ids.emplace_back(playbackCommandId(command));
    }

    return ids;
  }
} // namespace ao::uimodel
