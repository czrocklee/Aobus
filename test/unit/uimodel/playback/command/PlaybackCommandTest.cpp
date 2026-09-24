// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/playback/command/PlaybackCommand.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <set>
#include <string>
#include <string_view>

namespace ao::uimodel::test
{
  TEST_CASE("PlaybackCommand - every id resolves back to its command", "[uimodel][unit][playback][command]")
  {
    struct ExpectedMapping final
    {
      PlaybackCommand command;
      std::string_view id;
      std::string_view actionId;
    };

    constexpr auto kExpectedMappings = std::array{
      ExpectedMapping{PlaybackCommand::Play, "play", "playback.play"},
      ExpectedMapping{PlaybackCommand::Pause, "pause", "playback.pause"},
      ExpectedMapping{PlaybackCommand::PlayPause, "playPause", "playback.playPause"},
      ExpectedMapping{PlaybackCommand::Stop, "stop", "playback.stop"},
      ExpectedMapping{PlaybackCommand::Next, "next", "playback.next"},
      ExpectedMapping{PlaybackCommand::Previous, "previous", "playback.previous"},
      ExpectedMapping{PlaybackCommand::ToggleShuffle, "toggleShuffle", "playback.toggleShuffle"},
      ExpectedMapping{PlaybackCommand::CycleRepeat, "cycleRepeat", "playback.cycleRepeat"},
    };

    for (auto const& expected : kExpectedMappings)
    {
      CAPTURE(expected.id);
      auto const optCommand = playbackCommandFor(expected.id);
      REQUIRE(optCommand);
      CHECK(*optCommand == expected.command);
      CHECK(playbackCommandId(expected.command) == expected.id);
      CHECK(playbackCommandActionId(expected.command) == expected.actionId);
    }
  }

  TEST_CASE("PlaybackCommand - the whole transport is offered", "[uimodel][unit][playback][command]")
  {
    constexpr auto kExpectedCommands = std::array{
      PlaybackCommand::Play,
      PlaybackCommand::Pause,
      PlaybackCommand::PlayPause,
      PlaybackCommand::Stop,
      PlaybackCommand::Next,
      PlaybackCommand::Previous,
      PlaybackCommand::ToggleShuffle,
      PlaybackCommand::CycleRepeat,
    };
    constexpr auto kExpectedIds = std::array<std::string_view, 8>{
      "play", "pause", "playPause", "stop", "next", "previous", "toggleShuffle", "cycleRepeat"};
    auto const commands = playbackCommands();
    auto const ids = playbackCommandIds();
    auto const uniqueIds = std::set<std::string>{ids.begin(), ids.end()};

    // Layout documents consume this closed ordered inventory, so count-only and
    // self-round-trip checks are not enough to detect a duplicate or omission.
    CHECK(std::ranges::equal(commands, kExpectedCommands));
    CHECK(std::ranges::equal(ids, kExpectedIds));
    CHECK(uniqueIds.size() == kExpectedIds.size());
  }

  TEST_CASE("PlaybackCommand - an unknown id names nothing", "[uimodel][unit][playback][command]")
  {
    // The caller decides what to fall back to; resolution itself does not guess.
    CHECK_FALSE(playbackCommandFor(""));
    CHECK_FALSE(playbackCommandFor("shuffle"));
    CHECK_FALSE(playbackCommandFor("playback.playPause"));
  }
} // namespace ao::uimodel::test
