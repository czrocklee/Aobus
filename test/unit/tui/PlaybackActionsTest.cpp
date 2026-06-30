// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/PlaybackActions.h"

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/runtime/PlaybackServiceTestSupport.h"
#include "tui/Model.h"
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

    TrackListItem trackItem(TrackId const id)
    {
      return TrackListItem{.id = id};
    }
  } // namespace

  TEST_CASE("PlaybackActions - playSelected starts the bounded selected track", "[tui][unit][playback]")
  {
    auto fixture = rt::test::PlaybackFixture<rt::test::MockExecutor>{};
    primeOutput(fixture);

    auto first = rt::test::TrackSpec{.title = "First", .artist = "One"};
    auto second = rt::test::TrackSpec{.title = "Second", .artist = "Two"};
    auto const firstId = fixture.testLib.addTrack(first);
    auto const secondId = fixture.testLib.addTrack(second);
    auto const tracks = std::vector{trackItem(firstId), trackItem(secondId)};

    CHECK(playSelected(fixture.playbackService, tracks, -4, ListId{7}));
    CHECK(fixture.playbackService.state().trackId == firstId);
    CHECK(fixture.playbackService.state().sourceListId == ListId{7});
    CHECK(fixture.playbackService.state().trackTitle == "First");

    CHECK(playSelected(fixture.playbackService, tracks, 12, ListId{8}));
    CHECK(fixture.playbackService.state().trackId == secondId);
    CHECK(fixture.playbackService.state().sourceListId == ListId{8});
    CHECK(fixture.playbackService.state().trackTitle == "Second");
  }

  TEST_CASE("PlaybackActions - playSelected rejects an empty track list", "[tui][unit][playback]")
  {
    auto fixture = rt::test::PlaybackFixture<rt::test::MockExecutor>{};

    CHECK_FALSE(playSelected(fixture.playbackService, {}, 0, ListId{7}));
    CHECK(fixture.playbackService.state().trackId == kInvalidTrackId);
    CHECK(fixture.playbackService.state().sourceListId == kInvalidListId);
  }

  TEST_CASE("PlaybackActions - togglePlayback starts the selected track when idle", "[tui][unit][playback]")
  {
    auto fixture = rt::test::PlaybackFixture<rt::test::MockExecutor>{};
    primeOutput(fixture);

    auto spec = rt::test::TrackSpec{.title = "Toggle Target", .artist = "Switcher"};
    auto const trackId = fixture.testLib.addTrack(spec);
    auto const tracks = std::vector{trackItem(trackId)};

    CHECK(togglePlayback(fixture.playbackService, tracks, 0, ListId{9}));
    CHECK(fixture.playbackService.state().trackId == trackId);
    CHECK(fixture.playbackService.state().trackTitle == "Toggle Target");
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
    CHECK(fixture.playbackService.state().volume > 0.69F);
    CHECK(fixture.playbackService.state().volume < 0.71F);

    changeVolume(fixture.playbackService, 1.0F);
    CHECK(fixture.playbackService.state().volume == 1.0F);

    changeVolume(fixture.playbackService, -2.0F);
    CHECK(fixture.playbackService.state().volume == 0.0F);
  }
} // namespace ao::tui::test
