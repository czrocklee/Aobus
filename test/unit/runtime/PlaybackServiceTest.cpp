// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/runtime/PlaybackServiceTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace ao::rt::test
{
  TEST_CASE("PlaybackService playback - playTrack fails when track does not exist", "[runtime][unit][playback][play]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};

    CHECK_FALSE(fixture.playbackService.playTrack(TrackId{99999}, ListId{7}));
  }

  TEST_CASE("PlaybackService playback - playTrack resolves track metadata", "[runtime][unit][playback][play]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};

    // Prime the device list. The first notification auto-selects the default
    // output; the duplicate exercises the "already selected" early return, and the
    // empty list exercises the no-devices guard.
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.onDevicesChangedCb(fixture.status.devices);
    auto emptyStatus = fixture.status;
    emptyStatus.devices.clear();
    fixture.onDevicesChangedCb(emptyStatus.devices);

    auto spec = TrackSpec{};
    spec.title = "Playable Track";
    spec.artist = "Queue Artist";
    spec.album = "Queue Album";
    spec.duration = std::chrono::minutes{3};
    auto const trackId = fixture.testLib.addTrack(spec);

    CHECK(fixture.playbackService.playTrack(trackId, ListId{7}));
    CHECK(fixture.playbackService.state().trackId == trackId);
    CHECK(fixture.playbackService.state().sourceListId == ListId{7});
    CHECK(fixture.playbackService.state().trackTitle == "Playable Track");
    CHECK(fixture.playbackService.state().trackArtist == "Queue Artist");
    CHECK(fixture.playbackService.state().duration == std::chrono::minutes{3});
  }
} // namespace ao::rt::test
