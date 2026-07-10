// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/runtime/PlaybackServiceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/Property.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/flow/Graph.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/source/TrackSourceCache.h>

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
      LibraryChanges changes;
      TrackSourceCache trackSourceCache{libraryFixture.library(), changes};
      ViewService viewService{executor, libraryFixture.library(), trackSourceCache};
      NotificationService notificationService;
      std::unique_ptr<PlaybackService> playbackServicePtr =
        std::make_unique<PlaybackService>(executor, viewService, libraryFixture.library(), notificationService);
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

  TEST_CASE("PlaybackService selection - playSelectionInView fails with empty selection",
            "[runtime][unit][playback][selection]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};

    auto const result = fixture.viewService.createView({.listId = kInvalidListId}, true);
    TrackId const tid = fixture.playbackService.playSelectionInView(result.viewId);
    CHECK(tid == kInvalidTrackId);
  }

  TEST_CASE("PlaybackService selection - playSelectionInView fails when track does not exist",
            "[runtime][unit][playback][selection]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};

    auto const result = fixture.viewService.createView({.listId = kInvalidListId}, true);
    fixture.viewService.setSelection(result.viewId, {TrackId{99999}});

    TrackId const tid = fixture.playbackService.playSelectionInView(result.viewId);
    CHECK(tid == kInvalidTrackId);
  }

  TEST_CASE("PlaybackService selection - playSelectionInView fails with invalid view or selection",
            "[runtime][unit][playback][selection]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};

    TrackId const tid = fixture.playbackService.playSelectionInView(ViewId{999});
    CHECK(tid == kInvalidTrackId);
  }

  TEST_CASE("PlaybackService selection - playSelectionInView succeeds", "[runtime][unit][playback][selection]")
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

    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const trackId = fixture.libraryFixture.addTrack({.title = "A Track", .uri = fixturePath});
    auto const result = fixture.viewService.createView({.listId = kInvalidListId}, true);
    fixture.viewService.setSelection(result.viewId, {trackId});

    if (fixture.onGraphChangedCb)
    {
      fixture.onGraphChangedCb(audio::flow::Graph{});
    }

    TrackId const tid = fixture.playbackService.playSelectionInView(result.viewId);
    CHECK(tid == trackId);
    CHECK(fixture.playbackService.state().nowPlaying.trackId == trackId);

    if (fixture.renderTarget != nullptr)
    {
      fixture.renderTarget->handleRouteReady("mock_anchor");
      fixture.renderTarget->handleDrainComplete();
      fixture.renderTarget->handlePropertyChanged(audio::PropertySnapshot{
        .id = audio::PropertyId::Volume,
        .optValue = audio::PropertyValue{1.0F},
        .info = audio::PropertyInfo{.canRead = true,
                                    .canWrite = true,
                                    .isAvailable = true,
                                    .emitsChangeNotifications = false,
                                    .isHardwareAssisted = true},
      });
    }
  }

  TEST_CASE("PlaybackService selection - teardown tolerates pending engine notifications",
            "[runtime][regression][playback][lifecycle]")
  {
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto fixturePtr = std::make_unique<PlaybackFixture<MockExecutor>>();
    fixturePtr->onDevicesChangedCb(fixturePtr->status.devices);

    auto const trackId = fixturePtr->libraryFixture.addTrack({.title = "A Track", .uri = fixturePath});
    auto const result = fixturePtr->viewService.createView({.listId = kInvalidListId}, true);
    fixturePtr->viewService.setSelection(result.viewId, {trackId});

    REQUIRE(fixturePtr->playbackService.playSelectionInView(result.viewId) == trackId);
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
