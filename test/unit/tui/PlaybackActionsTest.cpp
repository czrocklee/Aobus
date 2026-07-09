// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/PlaybackActions.h"

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/PlaybackServiceTestSupport.h"
#include "tui/TrackListEntry.h"
#include <ao/CoreIds.h>
#include <ao/rt/PlaybackState.h>

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
    auto fixture = rt::test::PlaybackFixture<rt::test::MockExecutor>{};
    primeOutput(fixture);

    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto first = library::test::TrackSpec{.title = "First", .artist = "One", .uri = fixturePath};
    auto second = library::test::TrackSpec{.title = "Second", .artist = "Two", .uri = fixturePath};
    auto const firstId = fixture.libraryFixture.addTrack(first);
    auto const secondId = fixture.libraryFixture.addTrack(second);
    auto const tracks = std::vector{trackEntry(firstId), trackEntry(secondId)};

    CHECK(playSelected(fixture.playbackService, tracks, -4, ListId{7}));
    CHECK(fixture.playbackService.state().nowPlaying.trackId == firstId);
    CHECK(fixture.playbackService.state().nowPlaying.sourceListId == ListId{7});
    CHECK(fixture.playbackService.state().nowPlaying.title == "First");

    CHECK(playSelected(fixture.playbackService, tracks, 12, ListId{8}));
    CHECK(fixture.playbackService.state().nowPlaying.trackId == secondId);
    CHECK(fixture.playbackService.state().nowPlaying.sourceListId == ListId{8});
    CHECK(fixture.playbackService.state().nowPlaying.title == "Second");
  }

  TEST_CASE("PlaybackActions - playSelected rejects an empty track list", "[tui][unit][playback]")
  {
    auto fixture = rt::test::PlaybackFixture<rt::test::MockExecutor>{};

    CHECK_FALSE(playSelected(fixture.playbackService, {}, 0, ListId{7}));
    CHECK(fixture.playbackService.state().nowPlaying.trackId == kInvalidTrackId);
    CHECK(fixture.playbackService.state().nowPlaying.sourceListId == kInvalidListId);
  }

  TEST_CASE("PlaybackActions - togglePlayback starts the selected track when idle", "[tui][unit][playback]")
  {
    auto fixture = rt::test::PlaybackFixture<rt::test::MockExecutor>{};
    primeOutput(fixture);

    auto spec = library::test::TrackSpec{
      .title = "Toggle Target",
      .artist = "Switcher",
      .uri = audio::test::requireAudioFixture("basic_metadata.flac").string(),
    };

    auto const trackId = fixture.libraryFixture.addTrack(spec);
    auto const tracks = std::vector{trackEntry(trackId)};

    CHECK(togglePlayback(fixture.playbackService, tracks, 0, ListId{9}));
    CHECK(fixture.playbackService.state().nowPlaying.trackId == trackId);
    CHECK(fixture.playbackService.state().nowPlaying.title == "Toggle Target");
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
