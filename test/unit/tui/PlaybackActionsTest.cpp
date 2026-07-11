// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/PlaybackActions.h"

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/runtime/PlaybackSequenceUiTestSupport.h"
#include "test/unit/runtime/PlaybackServiceTestSupport.h"
#include "tui/TrackListEntry.h"
#include <ao/CoreIds.h>
#include <ao/rt/PlaybackSequenceService.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/VirtualListIds.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <vector>

namespace ao::tui::test
{
  namespace
  {
    void primeOutput(rt::test::PlaybackFixture<rt::test::MockExecutor>& fixture)
    {
      fixture.onDevicesChangedCb(fixture.status.devices);
    }

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

  TEST_CASE("PlaybackActions - togglePlayback starts the selected track when idle", "[tui][unit][playback]")
  {
    auto fixture = rt::test::PlaybackSequenceUiFixture{};
    fixture.makePlaybackReady();
    auto const trackId = fixture.addPlayableTrack("Toggle Target");
    auto const tracks = std::vector{trackEntry(trackId)};

    CHECK(togglePlayback(fixture.runtime.playback(), fixture.runtime.playbackSequence(), tracks, 0, fixture.viewId));
    CHECK(fixture.runtime.playback().state().nowPlaying.trackId == trackId);
    CHECK(fixture.runtime.playback().state().nowPlaying.title == "Toggle Target");
  }

  TEST_CASE("PlaybackActions - seekBy clamps negative relative seeks", "[tui][unit][playback]")
  {
    auto fixture = rt::test::PlaybackFixture<rt::test::MockExecutor>{};
    auto seekEvents = std::vector<std::chrono::milliseconds>{};
    auto sub = fixture.playbackService.onSeekUpdate([&](rt::PlaybackService::SeekUpdate const& event)
                                                    { seekEvents.push_back(event.elapsed); });

    fixture.playbackService.seek(std::chrono::seconds{10});
    seekBy(fixture.playbackService, -std::chrono::seconds{15});
    REQUIRE(seekEvents.size() == 2);
    CHECK(seekEvents[1] == std::chrono::milliseconds{0});

    seekBy(fixture.playbackService, std::chrono::seconds{45});
    REQUIRE(seekEvents.size() == 3);
    CHECK(seekEvents[2] == std::chrono::seconds{45});
  }

  TEST_CASE("PlaybackActions - changeVolume clamps the resulting volume", "[tui][unit][playback]")
  {
    auto fixture = rt::test::PlaybackFixture<rt::test::MockExecutor>{};
    primeOutput(fixture);

    fixture.playbackService.setVolume(0.4F);
    changeVolume(fixture.playbackService, 0.3F);
    CHECK(fixture.playbackService.state().volume.level > 0.69F);
    CHECK(fixture.playbackService.state().volume.level < 0.71F);

    changeVolume(fixture.playbackService, 1.0F);
    CHECK(fixture.playbackService.state().volume.level == 1.0F);

    changeVolume(fixture.playbackService, -2.0F);
    CHECK(fixture.playbackService.state().volume.level == 0.0F);
  }
} // namespace ao::tui::test
