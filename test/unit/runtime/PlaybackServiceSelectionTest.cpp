// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/runtime/PlaybackServiceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/RenderTarget.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/ViewIds.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    struct DestroyablePlaybackServiceFixture final
    {
      MusicLibraryFixture libraryFixture;
      MockExecutor executor;
      NotificationService notificationService;
      std::unique_ptr<PlaybackService> playbackServicePtr =
        std::make_unique<PlaybackService>(executor, libraryFixture.library(), notificationService);
    };
  } // namespace

  TEST_CASE("PlaybackService selection - reveal track requests", "[runtime][unit][playback][selection]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};

    auto revealRequests = std::vector<PlaybackService::RevealTrackRequested>{};
    auto sub = fixture.playbackService.onRevealTrackRequested([&](auto const& ev) { revealRequests.push_back(ev); });
    fixture.playbackService.revealTrack(TrackId{42});
    REQUIRE(revealRequests.size() == 1);
    CHECK(revealRequests[0].trackId == TrackId{42});
    CHECK(revealRequests[0].preferredListId == kInvalidListId);
    CHECK(revealRequests[0].preferredViewId == kInvalidViewId);
  }

  TEST_CASE("PlaybackService selection - teardown tolerates pending engine notifications",
            "[runtime][regression][playback][lifecycle]")
  {
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto fixturePtr = std::make_unique<PlaybackFixture<MockExecutor>>();
    fixturePtr->onDevicesChangedCb(fixturePtr->status.devices);

    auto const trackId = fixturePtr->libraryFixture.addTrack({.title = "A Track", .uri = fixturePath});
    REQUIRE(fixturePtr->playbackService.playTrack(trackId, ListId{1}));
    REQUIRE(fixturePtr->renderTarget != nullptr);

    auto callbackEntered = AsyncTestState<bool>::create(false);
    auto callbackRelease = AsyncBarrier{};
    auto subscription = Subscription{};
    subscription = fixturePtr->playbackService.onQualityChanged(
      [&subscription, callbackEntered, &callbackRelease](PlaybackService::QualityChanged const&)
      {
        subscription.reset();
        callbackEntered.set(true);
        callbackRelease.wait();
      });

    fixturePtr->renderTarget->handleRouteReady("teardown-anchor");
    auto const callbackWasEntered = callbackEntered.waitUntil(true);

    if (!callbackWasEntered)
    {
      callbackRelease.release();
    }

    REQUIRE(callbackWasEntered);

    auto teardownStarted = AsyncTestState<bool>::create(false);
    auto teardownFuture = std::async(std::launch::async,
                                     [&fixturePtr, teardownStarted]
                                     {
                                       teardownStarted.set(true);
                                       fixturePtr.reset();
                                     });
    auto const teardownWasStarted = teardownStarted.waitUntil(true);
    CHECK(teardownWasStarted);

    if (teardownWasStarted)
    {
      CHECK(teardownFuture.wait_for(std::chrono::milliseconds{0}) == std::future_status::timeout);
    }

    callbackRelease.release();
    REQUIRE(teardownFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
  }

  TEST_CASE("PlaybackService selection - output callback can destroy its service",
            "[runtime][regression][playback][lifecycle]")
  {
    auto fixture = DestroyablePlaybackServiceFixture{};
    auto subscription = Subscription{};
    bool callbackReturned = false;
    subscription = fixture.playbackServicePtr->onOutputDeviceChanged(
      [&](OutputDeviceSelection const&)
      {
        subscription.reset();
        fixture.playbackServicePtr.reset();
        callbackReturned = true;
      });

    auto* const playbackService = fixture.playbackServicePtr.get();
    playbackService->addProvider(makeReadyAudioProvider());

    CHECK(callbackReturned);
    CHECK(fixture.playbackServicePtr == nullptr);
  }

  TEST_CASE("PlaybackService selection - preparing callback destruction cancels play",
            "[runtime][regression][playback][lifecycle]")
  {
    auto fixture = DestroyablePlaybackServiceFixture{};
    fixture.playbackServicePtr->addProvider(makeReadyAudioProvider());

    auto subscription = Subscription{};
    subscription = fixture.playbackServicePtr->onPreparing(
      [&]
      {
        subscription.reset();
        fixture.playbackServicePtr.reset();
      });

    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const request =
      playbackRequest(TrackId{1}, fixturePath.string(), "A Track", "An Artist", std::chrono::minutes{1});
    auto* const playbackService = fixture.playbackServicePtr.get();
    auto const result = playbackService->play(request, ListId{1});

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::InvalidState);
    CHECK(fixture.playbackServicePtr == nullptr);
  }

  TEST_CASE("PlaybackService selection - started callback can destroy its accepted facade",
            "[runtime][regression][playback][lifecycle]")
  {
    auto fixture = DestroyablePlaybackServiceFixture{};
    fixture.playbackServicePtr->addProvider(makeReadyAudioProvider());
    auto subscription = Subscription{};
    subscription = fixture.playbackServicePtr->onStarted(
      [&]
      {
        subscription.reset();
        fixture.playbackServicePtr.reset();
      });
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const request =
      playbackRequest(TrackId{1}, fixturePath.string(), "A Track", "An Artist", std::chrono::minutes{1});
    auto* const playbackService = fixture.playbackServicePtr.get();

    auto const result = playbackService->play(request, ListId{1});

    REQUIRE(result);
    CHECK(result->trackId == TrackId{1});
    CHECK(fixture.playbackServicePtr == nullptr);
  }

  TEST_CASE("PlaybackService selection - restored now-playing callback can destroy its accepted facade",
            "[runtime][regression][playback][lifecycle]")
  {
    auto fixture = DestroyablePlaybackServiceFixture{};
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const trackId = fixture.libraryFixture.addTrack({.title = "Restored", .uri = fixturePath.string()});
    auto subscription = Subscription{};
    subscription = fixture.playbackServicePtr->onNowPlayingChanged(
      [&](PlaybackService::NowPlayingChanged const&)
      {
        subscription.reset();
        fixture.playbackServicePtr.reset();
      });
    auto* const playbackService = fixture.playbackServicePtr.get();

    auto const result = PlaybackServiceTestAccess::restoreSession(*playbackService,
                                                                  PlaybackTransportSessionState{
                                                                    .sourceListId = ListId{1},
                                                                    .trackId = trackId,
                                                                    .positionMs = 500,
                                                                  });

    REQUIRE(result);
    CHECK(fixture.playbackServicePtr == nullptr);
  }

  TEST_CASE("PlaybackService selection - revealPlayingTrack works", "[runtime][unit][playback][selection]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};

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
