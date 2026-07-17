// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/PlaybackActions.h"

#include "test/unit/runtime/PlaybackSequenceUiTestSupport.h"
#include "tui/TrackListEntry.h"
#include <ao/CoreIds.h>
#include <ao/rt/PlaybackSequenceService.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/VirtualListIds.h>

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
    auto fixture = rt::test::PlaybackSequenceUiFixture{};
    fixture.makePlaybackReady();
    auto const firstId = fixture.addPlayableTrack("First");
    auto const secondId = fixture.addPlayableTrack("Second");
    auto const tracks = std::vector{trackEntry(firstId), trackEntry(secondId)};
    auto& sequence = fixture.runtime.playbackSequence();
    auto& playback = fixture.runtime.playback();

    CHECK(playSelected(sequence, tracks, -4, fixture.viewId));
    CHECK(playback.state().nowPlaying.trackId == firstId);
    CHECK(playback.state().nowPlaying.sourceListId == rt::kAllTracksListId);
    CHECK(playback.state().nowPlaying.title == "First");
    CHECK(sequence.state().currentTrackId == firstId);

    CHECK(playSelected(sequence, tracks, 12, fixture.viewId));
    CHECK(playback.state().nowPlaying.trackId == secondId);
    CHECK(playback.state().nowPlaying.sourceListId == rt::kAllTracksListId);
    CHECK(playback.state().nowPlaying.title == "Second");
    CHECK(sequence.state().currentTrackId == secondId);
  }

  TEST_CASE("PlaybackActions - playSelected rejects an empty track list", "[tui][unit][playback]")
  {
    auto fixture = rt::test::PlaybackSequenceUiFixture{};

    CHECK_FALSE(playSelected(fixture.runtime.playbackSequence(), {}, 0, fixture.viewId));
    CHECK(fixture.runtime.playback().state().nowPlaying.trackId == kInvalidTrackId);
    CHECK(fixture.runtime.playbackSequence().state().currentTrackId == kInvalidTrackId);
  }
} // namespace ao::tui::test
