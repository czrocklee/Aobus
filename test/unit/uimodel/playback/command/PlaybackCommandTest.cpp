// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/playback/command/PlaybackCommand.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <string>

namespace ao::uimodel::test
{
  TEST_CASE("PlaybackCommand - every id resolves back to its command", "[uimodel][unit][playback]")
  {
    for (auto const& id : playbackCommandIds())
    {
      auto const optCommand = playbackCommandFor(id);
      REQUIRE(optCommand);
      CHECK(playbackCommandId(*optCommand) == id);
    }
  }

  TEST_CASE("PlaybackCommand - the whole transport is offered", "[uimodel][unit][playback]")
  {
    auto const ids = playbackCommandIds();

    // A document may name any command a shell can carry out, so the enumeration
    // and the offered ids cannot drift apart.
    CHECK(ids.size() == static_cast<std::size_t>(PlaybackCommand::CycleRepeat) + 1);
    CHECK(std::ranges::contains(ids, std::string{"playPause"}));
    CHECK(std::ranges::contains(ids, std::string{"toggleShuffle"}));
  }

  TEST_CASE("PlaybackCommand - an unknown id names nothing", "[uimodel][unit][playback]")
  {
    // The caller decides what to fall back to; resolution itself does not guess.
    CHECK_FALSE(playbackCommandFor(""));
    CHECK_FALSE(playbackCommandFor("shuffle"));
    CHECK_FALSE(playbackCommandFor("playback.playPause"));
  }
} // namespace ao::uimodel::test
