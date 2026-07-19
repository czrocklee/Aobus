// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/playback/PlaybackTransport.h"
#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/runtime/PlaybackTransportTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackFailure.h>
#include <ao/rt/PlaybackState.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <limits>

namespace ao::rt::test
{
  TEST_CASE("PlaybackTransport control - initial state is correct", "[runtime][unit][playback][control]")
  {
    auto fixture = PlaybackTransportFixture<InlineExecutor>{};

    auto const& state = fixture.playbackTransport.state();
    CHECK(state.nowPlaying == NowPlayingInfo{});
    CHECK(state.duration == std::chrono::milliseconds{0});
    CHECK(state.elapsed == std::chrono::milliseconds{0});
  }

  TEST_CASE("PlaybackTransport control - play pause resume and stop", "[runtime][unit][playback][control]")
  {
    auto fixture = PlaybackTransportFixture<InlineExecutor>{};

    // Prime the device list. The first notification auto-selects the default
    // output; the duplicate exercises the "already selected" early return, and the
    // empty list exercises the no-devices guard.
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.onDevicesChangedCb(fixture.status.devices);
    auto emptyStatus = fixture.status;
    emptyStatus.devices.clear();
    fixture.onDevicesChangedCb(emptyStatus.devices);

    bool preparingFired = false;
    auto subPrep = fixture.playbackTransport.onPreparing([&] { preparingFired = true; });

    bool startedFired = false;
    auto subStart = fixture.playbackTransport.onStarted([&] { startedFired = true; });

    bool nowPlayingFired = false;
    auto subNow = fixture.playbackTransport.onNowPlayingChanged(
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

    REQUIRE(fixture.playbackTransport.play(desc, ListId{1}));

    CHECK(preparingFired);
    CHECK(startedFired);
    CHECK(nowPlayingFired);
    CHECK(fixture.playbackTransport.state().nowPlaying == NowPlayingInfo{.trackId = TrackId{1},
                                                                         .sourceListId = ListId{1},
                                                                         .sourceViewId = ViewId{9},
                                                                         .coverArtId = ResourceId{77},
                                                                         .title = "Fake Track",
                                                                         .artist = "Fake Artist",
                                                                         .album = "Fake Album"});

    bool pausedFired = false;
    auto subPause = fixture.playbackTransport.onPaused([&] { pausedFired = true; });
    fixture.playbackTransport.pause();
    CHECK(pausedFired);

    startedFired = false;
    fixture.playbackTransport.resume();
    CHECK(startedFired);

    fixture.playbackTransport.seek(std::chrono::milliseconds{50});

    bool stoppedFired = false;
    auto subStop = fixture.playbackTransport.onStopped([&] { stoppedFired = true; });
    bool idleFired = false;
    auto subIdle = fixture.playbackTransport.onIdle([&] { idleFired = true; });

    fixture.playbackTransport.stop();
    CHECK(stoppedFired);
    CHECK(idleFired);
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == kInvalidTrackId);
  }

  TEST_CASE("PlaybackTransport control - seek updates state and fires event", "[runtime][unit][playback][control]")
  {
    auto fixture = PlaybackTransportFixture<InlineExecutor>{};

    bool seekFired = false;
    auto sub = fixture.playbackTransport.onSeekUpdate(
      [&](auto const& ev)
      {
        seekFired = true;
        CHECK(ev.elapsed == std::chrono::seconds{1});
        CHECK(ev.mode == PlaybackTransport::SeekMode::Final);
      });

    fixture.playbackTransport.seek(std::chrono::seconds{1}, PlaybackTransport::SeekMode::Final);
    CHECK(seekFired);
  }

  TEST_CASE("PlaybackTransport control - volume and muted controls update state", "[runtime][unit][playback][control]")
  {
    auto fixture = PlaybackTransportFixture<InlineExecutor>{};

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
    auto sub = fixture.playbackTransport.onMutedChanged(
      [&](bool const muted)
      {
        mutedChangedFired = true;
        lastMutedState = muted;
      });

    fixture.playbackTransport.setVolume(0.5F);
    CHECK(fixture.playbackTransport.state().volume.level == 0.5F);
    CHECK(fixture.playbackTransport.state().volume.hardwareAssisted == true);

    fixture.playbackTransport.setVolume(5.0F);
    CHECK(fixture.playbackTransport.state().volume.level == 1.0F);

    fixture.playbackTransport.setVolume(-1.0F);
    CHECK(fixture.playbackTransport.state().volume.level == 0.0F);

    fixture.playbackTransport.setVolume(std::numeric_limits<float>::quiet_NaN());
    CHECK(fixture.playbackTransport.state().volume.level == 1.0F);

    fixture.playbackTransport.setMuted(true);
    CHECK(fixture.playbackTransport.state().volume.muted == true);
    CHECK(mutedChangedFired);
    CHECK(lastMutedState == true);
  }

  TEST_CASE("PlaybackTransport control - commands refresh state synchronously", "[runtime][unit][playback][control]")
  {
    auto fixture = PlaybackTransportFixture<QueuedExecutor>{};

    // The provider callback is marshalled onto the executor, so the auto-select
    // does not take effect until the queued task drains.
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();
    CHECK(fixture.playbackTransport.state().ready);

    bool startedFired = false;
    auto subStarted = fixture.playbackTransport.onStarted([&] { startedFired = true; });
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const desc =
      playbackRequest(TrackId{77}, fixturePath, "Deferred Track", "Deferred Artist", std::chrono::minutes{4});

    REQUIRE(fixture.playbackTransport.play(desc, ListId{9}));

    // Control commands run on the executor thread (affinity contract), so they
    // refresh the state snapshot and emit their signals synchronously - no
    // executor turn is required, unlike the async Player callbacks.
    CHECK(startedFired);
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == TrackId{77});
    CHECK(fixture.playbackTransport.state().nowPlaying.sourceListId == ListId{9});

    // Draining any executor-deferred Player state callbacks leaves it intact.
    fixture.executor.drain();

    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == TrackId{77});
    CHECK(fixture.playbackTransport.state().nowPlaying.sourceListId == ListId{9});
  }

  TEST_CASE("PlaybackTransport control - rejected play does not emit success", "[runtime][unit][playback][control]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = InlineExecutor{};
    auto runtime = async::Runtime{executor, 1};
    auto notificationService = NotificationService{runtime};
    auto playbackTransport = makePlaybackTransport(executor, libraryFixture.library(), notificationService);

    bool preparingFired = false;
    auto subPreparing = playbackTransport.onPreparing([&] { preparingFired = true; });

    bool startedFired = false;
    auto subStarted = playbackTransport.onStarted([&] { startedFired = true; });

    bool nowPlayingFired = false;
    auto subNowPlaying = playbackTransport.onNowPlayingChanged([&](PlaybackTransport::NowPlayingChanged const&)
                                                               { nowPlayingFired = true; });

    bool failureFired = false;
    auto subFailure = playbackTransport.onPlaybackFailure([&](PlaybackFailure const&) { failureFired = true; });

    auto const desc =
      playbackRequest(TrackId{12}, "/not/started.flac", "Rejected Track", "Rejected Artist", std::chrono::minutes{5});

    auto const result = playbackTransport.play(desc, ListId{3});

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::InvalidState);
    CHECK(preparingFired);
    CHECK_FALSE(startedFired);
    CHECK_FALSE(nowPlayingFired);
    CHECK_FALSE(failureFired);
    CHECK(playbackTransport.state().nowPlaying.trackId == kInvalidTrackId);
    CHECK(playbackTransport.state().nowPlaying.sourceListId == kInvalidListId);
    CHECK(playbackTransport.state().nowPlaying.title.empty());
    auto const feed = notificationService.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().severity == NotificationSeverity::Error);
  }
} // namespace ao::rt::test
