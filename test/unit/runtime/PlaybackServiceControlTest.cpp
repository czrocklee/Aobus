// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/runtime/PlaybackServiceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/audio/Property.h>
#include <ao/audio/Transport.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackFailure.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackState.h>

#include <catch2/catch_test_macros.hpp>
#include <fakeit.hpp>

#include <chrono>
#include <cstdint>
#include <limits>

namespace ao::rt::test
{
  using namespace fakeit;

  namespace
  {
    constexpr bool withinOneMillisecond(std::chrono::milliseconds const actualElapsed,
                                        std::chrono::milliseconds const expectedElapsed)
    {
      auto const delta =
        actualElapsed > expectedElapsed ? actualElapsed - expectedElapsed : expectedElapsed - actualElapsed;
      return delta <= std::chrono::milliseconds{1};
    }
  } // namespace

  TEST_CASE("PlaybackService control - initial state is correct", "[runtime][unit][playback][control]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};

    auto const& state = fixture.playbackService.state();
    CHECK(state.nowPlaying == NowPlayingInfo{});
    CHECK(state.duration == std::chrono::milliseconds{0});
    CHECK(state.elapsed == std::chrono::milliseconds{0});
  }

  TEST_CASE("PlaybackService control - play pause resume and stop", "[runtime][unit][playback][control]")
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

    bool preparingFired = false;
    auto subPrep = fixture.playbackService.onPreparing([&] { preparingFired = true; });

    bool startedFired = false;
    auto subStart = fixture.playbackService.onStarted([&] { startedFired = true; });

    bool nowPlayingFired = false;
    auto subNow = fixture.playbackService.onNowPlayingChanged(
      [&](auto const& ev)
      {
        if (ev.trackId == TrackId{1})
        {
          nowPlayingFired = true;
        }
      });

    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const desc = playbackRequest(TrackId{1},
                                      fixturePath,
                                      "Fake Track",
                                      "Fake Artist",
                                      std::chrono::minutes{2},
                                      "Fake Album",
                                      ResourceId{77},
                                      ViewId{9});

    REQUIRE(fixture.playbackService.play(desc, ListId{1}));

    CHECK(preparingFired);
    CHECK(startedFired);
    CHECK(nowPlayingFired);
    CHECK(fixture.playbackService.state().nowPlaying == NowPlayingInfo{.trackId = TrackId{1},
                                                                       .sourceListId = ListId{1},
                                                                       .sourceViewId = ViewId{9},
                                                                       .coverArtId = ResourceId{77},
                                                                       .title = "Fake Track",
                                                                       .artist = "Fake Artist",
                                                                       .album = "Fake Album"});

    bool pausedFired = false;
    auto subPause = fixture.playbackService.onPaused([&] { pausedFired = true; });
    fixture.playbackService.pause();
    CHECK(pausedFired);

    startedFired = false;
    fixture.playbackService.resume();
    CHECK(startedFired);

    fixture.playbackService.seek(std::chrono::milliseconds{50});

    bool stoppedFired = false;
    auto subStop = fixture.playbackService.onStopped([&] { stoppedFired = true; });
    bool idleFired = false;
    auto subIdle = fixture.playbackService.onIdle([&] { idleFired = true; });

    fixture.playbackService.stop();
    CHECK(stoppedFired);
    CHECK(idleFired);
    CHECK(fixture.playbackService.state().nowPlaying.trackId == kInvalidTrackId);

    auto const stoppedSession = PlaybackServiceTestAccess::sessionState(fixture.playbackService);
    CHECK(stoppedSession.trackId == TrackId{1});
    CHECK(stoppedSession.sourceListId == ListId{1});
    CHECK(stoppedSession.positionMs == 50);
  }

  TEST_CASE("PlaybackService control - seek updates state and fires event", "[runtime][unit][playback][control]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};

    bool seekFired = false;
    auto sub = fixture.playbackService.onSeekUpdate(
      [&](auto const& ev)
      {
        seekFired = true;
        CHECK(ev.elapsed == std::chrono::seconds{1});
        CHECK(ev.mode == PlaybackService::SeekMode::Final);
      });

    fixture.playbackService.seek(std::chrono::seconds{1}, PlaybackService::SeekMode::Final);
    CHECK(seekFired);
  }

  TEST_CASE("PlaybackService control - volume and muted controls update state", "[runtime][unit][playback][control]")
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

    bool mutedChangedFired = false;
    bool lastMutedState = false;
    auto sub = fixture.playbackService.onMutedChanged(
      [&](bool const muted)
      {
        mutedChangedFired = true;
        lastMutedState = muted;
      });

    fixture.playbackService.setVolume(0.5F);
    CHECK(fixture.playbackService.state().volume.level == 0.5F);
    CHECK(fixture.playbackService.state().volume.hardwareAssisted == true);

    fixture.playbackService.setVolume(5.0F);
    CHECK(fixture.playbackService.state().volume.level == 1.0F);

    fixture.playbackService.setVolume(-1.0F);
    CHECK(fixture.playbackService.state().volume.level == 0.0F);

    fixture.playbackService.setVolume(std::numeric_limits<float>::quiet_NaN());
    CHECK(fixture.playbackService.state().volume.level == 1.0F);

    fixture.playbackService.setMuted(true);
    CHECK(fixture.playbackService.state().volume.muted == true);
    CHECK(mutedChangedFired);
    CHECK(lastMutedState == true);
  }

  TEST_CASE("PlaybackService control - commands refresh state synchronously", "[runtime][unit][playback][control]")
  {
    auto fixture = PlaybackFixture<QueuedExecutor>{};

    // The provider callback is marshalled onto the executor, so the auto-select
    // does not take effect until the queued task drains.
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();
    CHECK(fixture.playbackService.state().ready);

    bool startedFired = false;
    auto subStarted = fixture.playbackService.onStarted([&] { startedFired = true; });
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const desc =
      playbackRequest(TrackId{77}, fixturePath, "Deferred Track", "Deferred Artist", std::chrono::minutes{4});

    REQUIRE(fixture.playbackService.play(desc, ListId{9}));

    // Control commands run on the executor thread (affinity contract), so they
    // refresh the state snapshot and emit their signals synchronously - no
    // executor turn is required, unlike the async Player callbacks.
    CHECK(startedFired);
    CHECK(fixture.playbackService.state().nowPlaying.trackId == TrackId{77});
    CHECK(fixture.playbackService.state().nowPlaying.sourceListId == ListId{9});

    // Draining any executor-deferred Player state callbacks leaves it intact.
    fixture.executor.drain();

    CHECK(fixture.playbackService.state().nowPlaying.trackId == TrackId{77});
    CHECK(fixture.playbackService.state().nowPlaying.sourceListId == ListId{9});
  }

  TEST_CASE("PlaybackService control - rejected play does not emit success", "[runtime][unit][playback][control]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = MockExecutor{};
    auto notificationService = NotificationService{};
    auto playbackService = PlaybackService{executor, libraryFixture.library(), notificationService};

    bool preparingFired = false;
    auto subPreparing = playbackService.onPreparing([&] { preparingFired = true; });

    bool startedFired = false;
    auto subStarted = playbackService.onStarted([&] { startedFired = true; });

    bool nowPlayingFired = false;
    auto subNowPlaying =
      playbackService.onNowPlayingChanged([&](PlaybackService::NowPlayingChanged const&) { nowPlayingFired = true; });

    bool failureFired = false;
    auto subFailure = playbackService.onPlaybackFailure([&](PlaybackFailure const&) { failureFired = true; });

    auto const desc =
      playbackRequest(TrackId{12}, "/not/started.flac", "Rejected Track", "Rejected Artist", std::chrono::minutes{5});

    auto const result = playbackService.play(desc, ListId{3});

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::InvalidState);
    CHECK(preparingFired);
    CHECK_FALSE(startedFired);
    CHECK_FALSE(nowPlayingFired);
    CHECK_FALSE(failureFired);
    CHECK(playbackService.state().nowPlaying.trackId == kInvalidTrackId);
    CHECK(playbackService.state().nowPlaying.sourceListId == kInvalidListId);
    CHECK(playbackService.state().nowPlaying.title.empty());
    auto const feed = notificationService.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().severity == NotificationSeverity::Error);
  }

  TEST_CASE("PlaybackService control - restores deferred session without opening backend",
            "[runtime][unit][playback-control][session]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const trackId = fixture.libraryFixture.addTrack({.title = "Restored Track", .uri = fixturePath});
    auto const session = PlaybackTransportSessionState{
      .sourceListId = ListId{10},
      .trackId = trackId,
      .positionMs = 50,
      .volume = 0.5F,
      .muted = true,
    };

    REQUIRE(PlaybackServiceTestAccess::restoreSession(fixture.playbackService, session));

    auto const restored = fixture.playbackService.state();
    CHECK(restored.transport == audio::Transport::Idle);
    CHECK(restored.nowPlaying.trackId == trackId);
    CHECK(restored.nowPlaying.sourceListId == ListId{10});
    CHECK(restored.nowPlaying.title == "Restored Track");
    CHECK(restored.elapsed == std::chrono::milliseconds{50});
    CHECK(restored.volume.level == 0.5F);
    CHECK(restored.volume.muted == true);
    CHECK(fixture.renderTarget == nullptr);
    Verify(Method(fixture.spyBackendPtr->mock(), open)).Never();

    bool deferredSeekFired = false;
    auto subSeek = fixture.playbackService.onSeekUpdate(
      [&](PlaybackService::SeekUpdate const& event)
      {
        if (event.elapsed == std::chrono::milliseconds{75})
        {
          deferredSeekFired = true;
          CHECK(event.mode == PlaybackService::SeekMode::Final);
        }
      });

    fixture.playbackService.seek(std::chrono::milliseconds{75});

    CHECK(deferredSeekFired);
    CHECK(fixture.playbackService.state().elapsed == std::chrono::milliseconds{75});

    auto const snapshot = PlaybackServiceTestAccess::sessionState(fixture.playbackService);
    CHECK(snapshot.sourceListId == ListId{10});
    CHECK(snapshot.trackId == trackId);
    CHECK(snapshot.positionMs == 75);
    CHECK(snapshot.volume == 0.5F);
    CHECK(snapshot.muted == true);

    fixture.onDevicesChangedCb(fixture.status.devices);
    CHECK(fixture.renderTarget == nullptr);
    Verify(Method(fixture.spyBackendPtr->mock(), open)).Never();

    fixture.playbackService.resume();

    CHECK(fixture.renderTarget != nullptr);
    CHECK(fixture.playbackService.state().transport == audio::Transport::Playing);
    CHECK(fixture.playbackService.state().nowPlaying.trackId == trackId);
    CHECK(withinOneMillisecond(fixture.playbackService.state().elapsed, std::chrono::milliseconds{75}));
    Verify(Method(fixture.spyBackendPtr->mock(), open)).Once();
  }

  TEST_CASE("PlaybackService control - sanitizes restored session values", "[runtime][unit][playback-control][session]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const trackId = fixture.libraryFixture.addTrack(
      {.title = "Restored Track", .uri = fixturePath, .duration = std::chrono::seconds{3}});
    auto const session = PlaybackTransportSessionState{
      .sourceListId = ListId{10},
      .trackId = trackId,
      .positionMs = std::numeric_limits<std::uint64_t>::max(),
      .volume = 5.0F,
      .muted = true,
    };

    REQUIRE(PlaybackServiceTestAccess::restoreSession(fixture.playbackService, session));

    auto const restored = fixture.playbackService.state();
    CHECK(restored.nowPlaying.trackId == trackId);
    CHECK(restored.elapsed == std::chrono::milliseconds{0});
    CHECK(restored.volume.level == 1.0F);
    CHECK(restored.volume.muted == true);

    auto const snapshot = PlaybackServiceTestAccess::sessionState(fixture.playbackService);
    CHECK(snapshot.positionMs == 0);
    CHECK(snapshot.volume == 1.0F);
  }

  TEST_CASE("PlaybackService control - restore failure does not overwrite live restorable state",
            "[runtime][unit][playback-control][session]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const trackId = fixture.libraryFixture.addTrack({.title = "Restored Track", .uri = fixturePath});
    auto const storedSession = PlaybackTransportSessionState{
      .sourceListId = ListId{10},
      .trackId = trackId,
      .positionMs = 1234,
      .volume = 0.25F,
      .muted = true,
    };
    REQUIRE(PlaybackServiceTestAccess::restoreSession(fixture.playbackService, storedSession));

    auto const restoreResult = PlaybackServiceTestAccess::restoreSession(fixture.playbackService,
                                                                         PlaybackTransportSessionState{
                                                                           .sourceListId = ListId{10},
                                                                           .trackId = TrackId{9999},
                                                                           .positionMs = 5678,
                                                                         });
    CHECK_FALSE(restoreResult);

    auto const snapshot = PlaybackServiceTestAccess::sessionState(fixture.playbackService);
    CHECK(snapshot.sourceListId == storedSession.sourceListId);
    CHECK(snapshot.trackId == storedSession.trackId);
    CHECK(snapshot.positionMs == storedSession.positionMs);
    CHECK(snapshot.volume == storedSession.volume);
    CHECK(snapshot.muted == storedSession.muted);
  }

  TEST_CASE("PlaybackService control - property failure rolls restored volume and mute back atomically",
            "[runtime][unit][playback-control][session]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const trackId = fixture.libraryFixture.addTrack({.title = "Restored Track", .uri = fixturePath});
    auto const baselineRequest = PlaybackTransportSessionState{
      .sourceListId = ListId{10},
      .trackId = trackId,
      .positionMs = 250,
      .volume = 0.25F,
      .muted = false,
    };
    REQUIRE(PlaybackServiceTestAccess::restoreSession(fixture.playbackService, baselineRequest));
    auto const baseline = PlaybackServiceTestAccess::sessionState(fixture.playbackService);
    auto const baselinePlayback = fixture.playbackService.state();

    SECTION("volume failure leaves the baseline untouched")
    {
      When(Method(fixture.spyBackendPtr->mock(), setProperty))
        .AlwaysDo(
          [](audio::PropertyId const id, audio::PropertyValue const&) -> Result<>
          {
            if (id == audio::PropertyId::Volume)
            {
              return makeError(Error::Code::IoError, "volume property rejected");
            }

            return {};
          });
    }

    SECTION("mute failure rolls back the staged volume")
    {
      When(Method(fixture.spyBackendPtr->mock(), setProperty))
        .AlwaysDo(
          [](audio::PropertyId const id, audio::PropertyValue const&) -> Result<>
          {
            if (id == audio::PropertyId::Muted)
            {
              return makeError(Error::Code::IoError, "mute property rejected");
            }

            return {};
          });
    }

    auto const failed = PlaybackServiceTestAccess::restoreSession(fixture.playbackService,
                                                                  PlaybackTransportSessionState{
                                                                    .sourceListId = ListId{20},
                                                                    .trackId = trackId,
                                                                    .positionMs = 750,
                                                                    .volume = 0.75F,
                                                                    .muted = true,
                                                                  });

    REQUIRE_FALSE(failed);
    auto const snapshot = PlaybackServiceTestAccess::sessionState(fixture.playbackService);
    CHECK(snapshot.sourceListId == baseline.sourceListId);
    CHECK(snapshot.trackId == baseline.trackId);
    CHECK(snapshot.positionMs == baseline.positionMs);
    CHECK(snapshot.volume == baseline.volume);
    CHECK(snapshot.muted == baseline.muted);
    auto const& playback = fixture.playbackService.state();
    CHECK(playback.nowPlaying == baselinePlayback.nowPlaying);
    CHECK(playback.transport == baselinePlayback.transport);
    CHECK(playback.elapsed == baselinePlayback.elapsed);
    CHECK(playback.volume == baselinePlayback.volume);
  }
} // namespace ao::rt::test
