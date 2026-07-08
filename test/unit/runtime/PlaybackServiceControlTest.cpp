// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/runtime/PlaybackServiceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/audio/Transport.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackFailure.h>
#include <ao/rt/PlaybackSessionState.h>
#include <ao/rt/PlaybackState.h>

#include <catch2/catch_test_macros.hpp>
#include <fakeit.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
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
    CHECK(state.mode.shuffle == ShuffleMode::Off);
    CHECK(state.mode.repeat == RepeatMode::Off);
  }

  TEST_CASE("PlaybackService control - shuffle and repeat update state and emit signals",
            "[runtime][unit][playback][control]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};

    std::int32_t shuffleChangeCount = 0;
    auto sub1 = fixture.playbackService.onShuffleModeChanged(
      [&](auto const& ev)
      {
        ++shuffleChangeCount;
        CHECK(ev.mode == ShuffleMode::On);
      });

    fixture.playbackService.setShuffleMode(ShuffleMode::On);
    fixture.playbackService.setShuffleMode(ShuffleMode::On);
    CHECK(shuffleChangeCount == 1);
    CHECK(fixture.playbackService.state().mode.shuffle == ShuffleMode::On);

    std::int32_t repeatChangeCount = 0;
    auto sub2 = fixture.playbackService.onRepeatModeChanged(
      [&](auto const& ev)
      {
        ++repeatChangeCount;
        CHECK(ev.mode == RepeatMode::All);
      });

    fixture.playbackService.setRepeatMode(RepeatMode::All);
    fixture.playbackService.setRepeatMode(RepeatMode::All);
    CHECK(repeatChangeCount == 1);
    CHECK(fixture.playbackService.state().mode.repeat == RepeatMode::All);
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

    auto const stoppedSession = fixture.playbackService.sessionState();
    CHECK(stoppedSession.trackId == TrackId{1});
    CHECK(stoppedSession.sourceListId == ListId{1});
    CHECK(stoppedSession.positionMs == 50);

    auto const tempDir = ao::test::TempDir{};
    auto sessionStore = ConfigStore{std::filesystem::path{tempDir.path()} / "workspace.yaml"};
    fixture.playbackService.saveSession(sessionStore);

    auto reloadedStore = ConfigStore{std::filesystem::path{tempDir.path()} / "workspace.yaml"};
    auto reloadedSession = PlaybackSessionState{};
    REQUIRE(reloadedStore.load(kPlaybackSessionConfigGroup, reloadedSession));
    CHECK(reloadedSession.trackId == TrackId{1});
    CHECK(reloadedSession.sourceListId == ListId{1});
    CHECK(reloadedSession.positionMs == 50);
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
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto changes = LibraryChanges{};
    auto listSourceStore = ListSourceStore{testLib.library(), changes};
    auto viewService = ViewService{executor, testLib.library(), listSourceStore};
    auto notificationService = NotificationService{};
    auto playbackService = PlaybackService{executor, viewService, testLib.library(), notificationService};

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
    auto const trackId = fixture.testLib.addTrack({.title = "Restored Track", .uri = fixturePath});
    auto const session = PlaybackSessionState{
      .sourceListId = ListId{10},
      .trackId = trackId,
      .positionMs = 50,
      .shuffleMode = ShuffleMode::On,
      .repeatMode = RepeatMode::All,
      .volume = 0.5F,
      .muted = true,
    };

    REQUIRE(fixture.playbackService.restoreSession(session));

    auto const restored = fixture.playbackService.state();
    CHECK(restored.transport == audio::Transport::Idle);
    CHECK(restored.nowPlaying.trackId == trackId);
    CHECK(restored.nowPlaying.sourceListId == ListId{10});
    CHECK(restored.nowPlaying.title == "Restored Track");
    CHECK(restored.elapsed == std::chrono::milliseconds{50});
    CHECK(restored.mode.shuffle == ShuffleMode::On);
    CHECK(restored.mode.repeat == RepeatMode::All);
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

    auto const tempDir = ao::test::TempDir{};
    auto sessionStore = ConfigStore{std::filesystem::path{tempDir.path()} / "workspace.yaml"};
    fixture.playbackService.saveSession(sessionStore);

    auto reloadedStore = ConfigStore{std::filesystem::path{tempDir.path()} / "workspace.yaml"};
    auto reloadedSession = PlaybackSessionState{};
    REQUIRE(reloadedStore.load(kPlaybackSessionConfigGroup, reloadedSession));
    CHECK(reloadedSession.sourceListId == ListId{10});
    CHECK(reloadedSession.trackId == trackId);
    CHECK(reloadedSession.positionMs == 75);
    CHECK(reloadedSession.shuffleMode == ShuffleMode::On);
    CHECK(reloadedSession.repeatMode == RepeatMode::All);
    CHECK(reloadedSession.volume == 0.5F);
    CHECK(reloadedSession.muted == true);

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
    auto const trackId =
      fixture.testLib.addTrack({.title = "Restored Track", .uri = fixturePath, .duration = std::chrono::seconds{3}});
    auto const session = PlaybackSessionState{
      .sourceListId = ListId{10},
      .trackId = trackId,
      .positionMs = std::numeric_limits<std::uint64_t>::max(),
      .shuffleMode = static_cast<ShuffleMode>(99),
      .repeatMode = static_cast<RepeatMode>(99),
      .volume = 5.0F,
      .muted = true,
    };

    REQUIRE(fixture.playbackService.restoreSession(session));

    auto const restored = fixture.playbackService.state();
    CHECK(restored.nowPlaying.trackId == trackId);
    CHECK(restored.elapsed == std::chrono::seconds{3});
    CHECK(restored.mode.shuffle == ShuffleMode::Off);
    CHECK(restored.mode.repeat == RepeatMode::Off);
    CHECK(restored.volume.level == 1.0F);
    CHECK(restored.volume.muted == true);

    auto const tempDir = ao::test::TempDir{};
    auto sessionStore = ConfigStore{std::filesystem::path{tempDir.path()} / "workspace.yaml"};
    fixture.playbackService.saveSession(sessionStore);

    auto reloadedStore = ConfigStore{std::filesystem::path{tempDir.path()} / "workspace.yaml"};
    auto reloadedSession = PlaybackSessionState{};
    REQUIRE(reloadedStore.load(kPlaybackSessionConfigGroup, reloadedSession));
    CHECK(reloadedSession.positionMs == 0);
    CHECK(reloadedSession.shuffleMode == ShuffleMode::Off);
    CHECK(reloadedSession.repeatMode == RepeatMode::Off);
    CHECK(reloadedSession.volume == 1.0F);
  }

  TEST_CASE("PlaybackService control - restore failure does not overwrite persisted session",
            "[runtime][unit][playback-control][session]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const trackId = fixture.testLib.addTrack({.title = "Restored Track", .uri = fixturePath});
    auto const tempDir = ao::test::TempDir{};
    auto sessionStore = ConfigStore{std::filesystem::path{tempDir.path()} / "workspace.yaml"};
    auto const storedSession = PlaybackSessionState{
      .sourceListId = ListId{10},
      .trackId = TrackId{42},
      .positionMs = 1234,
      .shuffleMode = ShuffleMode::On,
      .repeatMode = RepeatMode::All,
      .volume = 0.25F,
      .muted = true,
    };
    sessionStore.save(kPlaybackSessionConfigGroup, storedSession);
    REQUIRE(sessionStore.flush());

    auto const schemaResult = fixture.playbackService.restoreSession(PlaybackSessionState{
      .schemaVersion = kPlaybackSessionSchemaVersion + 1,
      .sourceListId = ListId{10},
      .trackId = trackId,
      .positionMs = 5678,
    });
    REQUIRE_FALSE(schemaResult);
    CHECK(schemaResult.error().code == Error::Code::FormatRejected);

    auto const restoreResult = fixture.playbackService.restoreSession(PlaybackSessionState{
      .sourceListId = ListId{10},
      .trackId = TrackId{9999},
      .positionMs = 5678,
    });
    CHECK_FALSE(restoreResult);

    fixture.playbackService.saveSession(sessionStore);

    auto reloadedStore = ConfigStore{std::filesystem::path{tempDir.path()} / "workspace.yaml"};
    auto reloadedSession = PlaybackSessionState{};
    REQUIRE(reloadedStore.load(kPlaybackSessionConfigGroup, reloadedSession));
    CHECK(reloadedSession.sourceListId == storedSession.sourceListId);
    CHECK(reloadedSession.trackId == storedSession.trackId);
    CHECK(reloadedSession.positionMs == storedSession.positionMs);
    CHECK(reloadedSession.shuffleMode == storedSession.shuffleMode);
    CHECK(reloadedSession.repeatMode == storedSession.repeatMode);
    CHECK(reloadedSession.volume == storedSession.volume);
    CHECK(reloadedSession.muted == storedSession.muted);
  }
} // namespace ao::rt::test
