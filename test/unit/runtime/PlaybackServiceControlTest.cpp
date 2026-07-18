// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/runtime/PlaybackServiceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackFailure.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackState.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <limits>

namespace ao::rt::test
{
  TEST_CASE("PlaybackService control - initial state is correct", "[runtime][unit][playback][control]")
  {
    auto fixture = PlaybackFixture<InlineExecutor>{};

    auto const& state = fixture.playbackService.state();
    CHECK(state.nowPlaying == NowPlayingInfo{});
    CHECK(state.duration == std::chrono::milliseconds{0});
    CHECK(state.elapsed == std::chrono::milliseconds{0});
  }

  TEST_CASE("PlaybackService control - play pause resume and stop", "[runtime][unit][playback][control]")
  {
    auto fixture = PlaybackFixture<InlineExecutor>{};

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
  }

  TEST_CASE("PlaybackService control - seek updates state and fires event", "[runtime][unit][playback][control]")
  {
    auto fixture = PlaybackFixture<InlineExecutor>{};

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
    auto fixture = PlaybackFixture<InlineExecutor>{};

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
    auto executor = InlineExecutor{};
    auto notificationService = NotificationService{executor};
    auto playbackService = makePlaybackService(executor, libraryFixture.library(), notificationService);

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
} // namespace ao::rt::test
