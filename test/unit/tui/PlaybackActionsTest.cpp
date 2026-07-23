// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/PlaybackActions.h"

#include "test/unit/runtime/PlaybackUiTestSupport.h"
#include "tui/TrackListEntry.h"
#include <ao/CoreIds.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/playback/PlaybackService.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace ao::tui::test
{
  namespace
  {
    TrackListEntry trackEntry(TrackId const id)
    {
      return TrackListEntry{.id = id};
    }
  } // namespace

  TEST_CASE("PlaybackActions - playSelected starts the bounded selected track", "[tui][unit][playback]")
  {
    auto fixture = rt::test::PlaybackUiFixture{};
    fixture.makePlaybackReady();
    auto const firstId = fixture.addPlayableTrack("First");
    auto const secondId = fixture.addPlayableTrack("Second");
    auto const tracks = std::vector{trackEntry(firstId), trackEntry(secondId)};
    auto& playback = fixture.runtime.playback();
    auto& commands = playback.commands();

    CHECK(playSelected(commands, tracks, -4, fixture.viewId));
    REQUIRE(fixture.waitForPlayback(firstId));
    auto snapshot = playback.snapshot();
    CHECK(snapshot.transport.nowPlaying.trackId == firstId);
    CHECK(snapshot.transport.nowPlaying.sourceListId == rt::kAllTracksListId);
    CHECK(snapshot.transport.nowPlaying.title == "First");
    CHECK(snapshot.succession.currentTrackId == firstId);

    CHECK(playSelected(commands, tracks, 12, fixture.viewId));
    REQUIRE(fixture.waitForPlayback(secondId));
    snapshot = playback.snapshot();
    CHECK(snapshot.transport.nowPlaying.trackId == secondId);
    CHECK(snapshot.transport.nowPlaying.sourceListId == rt::kAllTracksListId);
    CHECK(snapshot.transport.nowPlaying.title == "Second");
    CHECK(snapshot.succession.currentTrackId == secondId);
  }

  TEST_CASE("PlaybackActions - playSelected rejects an empty track list", "[tui][unit][playback]")
  {
    auto fixture = rt::test::PlaybackUiFixture{};
    auto& playback = fixture.runtime.playback();

    CHECK_FALSE(playSelected(playback.commands(), {}, 0, fixture.viewId));
    auto const snapshot = playback.snapshot();
    CHECK(snapshot.transport.nowPlaying.trackId == kInvalidTrackId);
    CHECK(snapshot.succession.currentTrackId == kInvalidTrackId);
  }
} // namespace ao::tui::test
