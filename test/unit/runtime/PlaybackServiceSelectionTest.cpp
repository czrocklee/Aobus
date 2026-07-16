// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/runtime/PlaybackServiceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/RenderTarget.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/ViewIds.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("PlaybackService selection - reveal track requests", "[runtime][unit][playback][selection]")
  {
    auto fixture = PlaybackFixture<InlineExecutor>{};

    auto revealRequests = std::vector<PlaybackService::RevealTrackRequested>{};
    auto sub = fixture.playbackService.onRevealTrackRequested([&](auto const& ev) { revealRequests.push_back(ev); });
    fixture.playbackService.revealTrack(TrackId{42});
    REQUIRE(revealRequests.size() == 1);
    CHECK(revealRequests[0].trackId == TrackId{42});
    CHECK(revealRequests[0].preferredListId == kInvalidListId);
    CHECK(revealRequests[0].preferredViewId == kInvalidViewId);
  }

  TEST_CASE("PlaybackService selection - teardown is deferred after pending engine notification",
            "[runtime][regression][playback][concurrency]")
  {
    auto fixturePtr = std::make_unique<PlaybackFixture<QueuedExecutor>>();
    fixturePtr->onDevicesChangedCb(fixturePtr->status.devices);
    fixturePtr->executor.drain();

    auto const fixtureUri = fixturePtr->installAudioFixture();
    auto const trackId = fixturePtr->libraryFixture.addTrack({.title = "A Track", .uri = fixtureUri});
    REQUIRE(fixturePtr->playbackService.playTrack(trackId, ListId{1}));
    REQUIRE(fixturePtr->renderTarget != nullptr);

    bool callbackEntered = false;
    auto subscription = Subscription{};
    subscription = fixturePtr->playbackService.onQualityChanged(
      [&subscription, &callbackEntered](PlaybackService::QualityChanged const&)
      {
        subscription.reset();
        callbackEntered = true;
      });

    fixturePtr->renderTarget->handleRouteReady("teardown-anchor");
    REQUIRE(fixturePtr->executor.drainUntil([&] { return callbackEntered; }));
    REQUIRE(fixturePtr);

    // Teardown happens on the owner thread only after callback publication has
    // unwound; queued Player work is invalidated by its lifetime gate.
    fixturePtr.reset();
    CHECK_FALSE(fixturePtr);
  }

  TEST_CASE("PlaybackService selection - revealPlayingTrack works", "[runtime][unit][playback][selection]")
  {
    auto fixture = PlaybackFixture<InlineExecutor>{};

    auto revealRequests = std::vector<PlaybackService::RevealTrackRequested>{};
    auto sub = fixture.playbackService.onRevealTrackRequested([&](PlaybackService::RevealTrackRequested const& ev)
                                                              { revealRequests.push_back(ev); });
    fixture.playbackService.revealPlayingTrack();
    REQUIRE(revealRequests.size() == 1);
    CHECK(revealRequests[0].trackId == kInvalidTrackId);
    CHECK(revealRequests[0].preferredListId == kInvalidListId);
    CHECK(revealRequests[0].preferredViewId == kInvalidViewId);
  }
} // namespace ao::rt::test
