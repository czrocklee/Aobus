// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/runtime/PlaybackServiceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/PlaybackState.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace ao::rt::test
{
  TEST_CASE("PlaybackService control - initial state is correct", "[runtime][unit][playback][control]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};

    auto const& state = fixture.playbackService.state();
    CHECK(state.trackId == kInvalidTrackId);
    CHECK(state.sourceListId == kInvalidListId);
    CHECK(state.duration == std::chrono::milliseconds{0});
    CHECK(state.elapsed == std::chrono::milliseconds{0});
    CHECK(state.shuffleMode == ShuffleMode::Off);
    CHECK(state.repeatMode == RepeatMode::Off);
  }

  TEST_CASE("PlaybackService control - shuffle and repeat update state and emit signals",
            "[runtime][unit][playback][control]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};

    bool shuffleChanged = false;
    auto sub1 = fixture.playbackService.onShuffleModeChanged(
      [&](auto const& ev)
      {
        shuffleChanged = true;
        CHECK(ev.mode == ShuffleMode::On);
      });

    fixture.playbackService.setShuffleMode(ShuffleMode::On);
    CHECK(shuffleChanged);
    CHECK(fixture.playbackService.state().shuffleMode == ShuffleMode::On);

    bool repeatChanged = false;
    auto sub2 = fixture.playbackService.onRepeatModeChanged(
      [&](auto const& ev)
      {
        repeatChanged = true;
        CHECK(ev.mode == RepeatMode::All);
      });

    fixture.playbackService.setRepeatMode(RepeatMode::All);
    CHECK(repeatChanged);
    CHECK(fixture.playbackService.state().repeatMode == RepeatMode::All);
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

    auto const desc =
      playbackRequest(TrackId{1}, "/fake/path.flac", "Fake Track", "Fake Artist", std::chrono::minutes{2});

    fixture.playbackService.play(desc, ListId{1});

    CHECK(preparingFired);
    CHECK(startedFired);
    CHECK(nowPlayingFired);
    CHECK(fixture.playbackService.state().trackId == TrackId{1});
    CHECK(fixture.playbackService.state().sourceListId == ListId{1});
    CHECK(fixture.playbackService.state().trackTitle == "Fake Track");
    CHECK(fixture.playbackService.state().trackArtist == "Fake Artist");

    bool pausedFired = false;
    auto subPause = fixture.playbackService.onPaused([&] { pausedFired = true; });
    fixture.playbackService.pause();
    CHECK(pausedFired);

    startedFired = false;
    fixture.playbackService.resume();
    CHECK(startedFired);

    bool stoppedFired = false;
    auto subStop = fixture.playbackService.onStopped([&] { stoppedFired = true; });
    bool idleFired = false;
    auto subIdle = fixture.playbackService.onIdle([&] { idleFired = true; });

    fixture.playbackService.stop();
    CHECK(stoppedFired);
    CHECK(idleFired);
    CHECK(fixture.playbackService.state().trackId == kInvalidTrackId);
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
    CHECK(fixture.playbackService.state().volume == 0.5F);
    CHECK(fixture.playbackService.state().volumeIsHardwareAssisted == true);

    fixture.playbackService.setMuted(true);
    CHECK(fixture.playbackService.state().muted == true);
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
    auto const desc =
      playbackRequest(TrackId{77}, "/fake/path.flac", "Deferred Track", "Deferred Artist", std::chrono::minutes{4});

    fixture.playbackService.play(desc, ListId{9});

    // Control commands run on the executor thread (affinity contract), so they
    // refresh the state snapshot and emit their signals synchronously - no
    // executor turn is required, unlike the async Player callbacks.
    CHECK(startedFired);
    CHECK(fixture.playbackService.state().trackId == TrackId{77});
    CHECK(fixture.playbackService.state().sourceListId == ListId{9});

    // Draining any executor-deferred Player state callbacks leaves it intact.
    fixture.executor.drain();

    CHECK(fixture.playbackService.state().trackId == TrackId{77});
    CHECK(fixture.playbackService.state().sourceListId == ListId{9});
  }

  TEST_CASE("PlaybackService control - rejected play does not emit success", "[runtime][unit][playback][control]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto changes = LibraryChanges{};
    auto listSourceStore = ListSourceStore{testLib.library(), changes};
    auto viewService = ViewService{executor, testLib.library(), listSourceStore};
    auto playbackService = PlaybackService{executor, viewService, testLib.library()};

    bool preparingFired = false;
    auto subPreparing = playbackService.onPreparing([&] { preparingFired = true; });

    bool startedFired = false;
    auto subStarted = playbackService.onStarted([&] { startedFired = true; });

    bool nowPlayingFired = false;
    auto subNowPlaying =
      playbackService.onNowPlayingChanged([&](PlaybackService::NowPlayingChanged const&) { nowPlayingFired = true; });

    auto const desc =
      playbackRequest(TrackId{12}, "/not/started.flac", "Rejected Track", "Rejected Artist", std::chrono::minutes{5});

    CHECK_FALSE(playbackService.play(desc, ListId{3}));

    CHECK(preparingFired);
    CHECK_FALSE(startedFired);
    CHECK_FALSE(nowPlayingFired);
    CHECK(playbackService.state().trackId == kInvalidTrackId);
    CHECK(playbackService.state().sourceListId == kInvalidListId);
    CHECK(playbackService.state().trackTitle.empty());
  }
} // namespace ao::rt::test
