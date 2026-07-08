// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/runtime/PlaybackServiceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/Transport.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/uimodel/playback/queue/PlaybackQueueSession.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::uimodel::test
{
  using namespace ao::rt::test;
  using namespace ao::rt;

  namespace
  {
    std::size_t notificationCount(NotificationService const& notifications,
                                  NotificationSeverity severity,
                                  std::string_view messageFragment)
    {
      std::size_t count = 0;

      for (auto const& entry : notifications.feed().entries)
      {
        if (entry.severity == severity && entry.message.find(messageFragment) != std::string::npos)
        {
          ++count;
        }
      }

      return count;
    }

    bool hasNotification(NotificationService const& notifications,
                         NotificationSeverity severity,
                         std::string_view messageFragment)
    {
      return notificationCount(notifications, severity, messageFragment) != 0;
    }
  } // namespace

  TEST_CASE("PlaybackQueueSession - basic controls", "[uimodel][unit][playback]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto changes = LibraryChanges{};
    auto trackSourceCache = TrackSourceCache{testLib.library(), changes};
    auto viewService = ViewService{executor, testLib.library(), trackSourceCache};
    auto notificationService = NotificationService{};

    auto playbackService = PlaybackService{executor, viewService, testLib.library(), notificationService};
    addReadyAudioProvider(playbackService);

    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const track1 = testLib.addTrack({.title = "Track 1", .uri = fixturePath});
    auto const track2 = testLib.addTrack({.title = "Track 2", .uri = fixturePath});
    auto const track3 = testLib.addTrack({.title = "Track 3", .uri = fixturePath});
    auto const missingTrack = TrackId{999};

    auto queueSession = PlaybackQueueSession{playbackService, notificationService};

    SECTION("initial state is inactive")
    {
      CHECK(queueSession.isActive() == false);
      CHECK(queueSession.nowPlayingTrackId() == std::nullopt);
      CHECK(queueSession.sourceListId() == kInvalidListId);
    }

    SECTION("resume handles inactive sequence")
    {
      queueSession.resume(); // Should not crash
    }

    SECTION("transport actions when inactive")
    {
      playbackService.setShuffleMode(rt::ShuffleMode::On);
      playbackService.setRepeatMode(rt::RepeatMode::One);
      queueSession.next();
      queueSession.previous();
      // Should not crash or activate
      CHECK(queueSession.isActive() == false);
    }

    SECTION("playQueue fails if startTrackId is not in the list")
    {
      auto const tracks = std::vector{track1, track2};
      auto const result = queueSession.playQueue(tracks, track3, ListId{10});
      CHECK(result == false);
      CHECK(queueSession.isActive() == false);
    }

    SECTION("playQueue sets up the queue correctly")
    {
      auto const tracks = std::vector{track1, track2, track3};
      auto const result = queueSession.playQueue(tracks, track2, ListId{10});

      CHECK(result == true);
      CHECK(queueSession.isActive() == true);
      CHECK(queueSession.nowPlayingTrackId() == track2);
      CHECK(queueSession.sourceListId() == ListId{10});
      CHECK(playbackService.state().nowPlaying.trackId == track2);
    }

    SECTION("playQueue fails on empty list")
    {
      auto const tracks = std::vector<TrackId>{};
      auto const result = queueSession.playQueue(tracks, track2, ListId{10});
      CHECK(result == false);
    }

    SECTION("playQueue fails on missing track")
    {
      auto const tracks = std::vector{missingTrack};
      auto const result = queueSession.playQueue(tracks, missingTrack, ListId{10});
      CHECK(result == false);
    }

    SECTION("advanceToNext when active and idle")
    {
      auto const tracks = std::vector{track1, track2, track3};
      queueSession.playQueue(tracks, track1, ListId{10});
      CHECK(queueSession.nowPlayingTrackId() == track1);

      queueSession.next();
      CHECK(queueSession.nowPlayingTrackId() == track2);

      queueSession.next();
      CHECK(queueSession.nowPlayingTrackId() == track3);

      // End of list, no repeat
      queueSession.next();
      CHECK(queueSession.isActive() == false);
    }

    SECTION("peekNext does not move the current cursor")
    {
      auto const tracks = std::vector{track1, track2, track3};
      queueSession.playQueue(tracks, track1, ListId{10});

      auto const optFirstPeek = queueSession.peekNext();
      auto const optSecondPeek = queueSession.peekNext();

      REQUIRE(optFirstPeek);
      CHECK(*optFirstPeek == track2);
      CHECK(optSecondPeek == optFirstPeek);
      CHECK(queueSession.nowPlayingTrackId() == track1);
      CHECK(playbackService.state().nowPlaying.trackId == track1);
    }

    SECTION("now-playing change commits pending successor")
    {
      auto const tracks = std::vector{track1, track2, track3};
      queueSession.playQueue(tracks, track1, ListId{10});

      REQUIRE(queueSession.peekNext() == track2);
      REQUIRE(playbackService.playTrack(track2, ListId{10}));

      CHECK(queueSession.nowPlayingTrackId() == track2);
      CHECK(queueSession.peekNext() == track3);
    }

    SECTION("now-playing change from another source list does not commit pending successor")
    {
      auto const tracks = std::vector{track1, track2, track3};
      queueSession.playQueue(tracks, track1, ListId{10});

      REQUIRE(queueSession.peekNext() == track2);
      REQUIRE(playbackService.playTrack(track2, ListId{99}));

      CHECK(queueSession.nowPlayingTrackId() == track1);
      CHECK(queueSession.peekNext() == track2);
      CHECK(queueSession.sourceListId() == ListId{10});
    }

    SECTION("previous when active")
    {
      auto const tracks = std::vector{track1, track2, track3};
      queueSession.playQueue(tracks, track2, ListId{10});
      CHECK(queueSession.nowPlayingTrackId() == track2);

      queueSession.previous();
      CHECK(queueSession.nowPlayingTrackId() == track1);

      // At start of list, no repeat
      queueSession.previous();
      CHECK(queueSession.nowPlayingTrackId() == track1); // doesn't wrap around
    }

    SECTION("Repeat One")
    {
      playbackService.setRepeatMode(rt::RepeatMode::One);
      auto const tracks1 = std::vector{track1, track2};
      queueSession.playQueue(tracks1, track1, ListId{10});

      queueSession.next();
      CHECK(queueSession.nowPlayingTrackId() == track1);
    }

    SECTION("Repeat All")
    {
      playbackService.setRepeatMode(rt::RepeatMode::All);
      auto const tracks2 = std::vector{track1, track2};
      queueSession.playQueue(tracks2, track2, ListId{10});

      queueSession.next();
      CHECK(queueSession.nowPlayingTrackId() == track1);

      queueSession.previous();
      CHECK(queueSession.nowPlayingTrackId() == track2);
    }

    SECTION("Previous at start with Repeat All wraps to end")
    {
      playbackService.setRepeatMode(rt::RepeatMode::All);
      auto const tracks = std::vector{track1, track2, track3};
      queueSession.playQueue(tracks, track1, ListId{10});
      CHECK(queueSession.nowPlayingTrackId() == track1);

      queueSession.previous();
      CHECK(queueSession.nowPlayingTrackId() == track3);
    }

    SECTION("Previous skips invalid tracks")
    {
      // Setup: [1, 999, 3] where 999 is invalid
      auto const tracks = std::vector{track1, missingTrack, track3};
      queueSession.playQueue(tracks, track3, ListId{10});
      CHECK(queueSession.nowPlayingTrackId() == track3);

      // From 3, go previous. Should skip 999 and find 1.
      queueSession.previous();
      CHECK(queueSession.nowPlayingTrackId() == track1);
    }

    SECTION("Previous at start with Repeat All skips invalid tracks at end")
    {
      playbackService.setRepeatMode(rt::RepeatMode::All);
      // Setup: [1, 2, 999] where 999 is invalid
      auto const tracks = std::vector{track1, track2, missingTrack};
      queueSession.playQueue(tracks, track1, ListId{10});
      CHECK(queueSession.nowPlayingTrackId() == track1);

      // From 1, go previous. Should wrap to end, skip 999, and find 2.
      queueSession.previous();
      CHECK(queueSession.nowPlayingTrackId() == track2);
    }

    SECTION("Shuffle mode next picks random track")
    {
      playbackService.setShuffleMode(rt::ShuffleMode::On);
      auto const tracks = std::vector{track1, track2, track3};
      queueSession.playQueue(tracks, track1, ListId{10});

      queueSession.next();
      CHECK(queueSession.isActive() == true);
    }

    SECTION("Shuffle mode next commits the optPeeked successor")
    {
      playbackService.setShuffleMode(rt::ShuffleMode::On);
      auto const tracks = std::vector{track1, track2, track3};
      queueSession.playQueue(tracks, track1, ListId{10});

      auto const optPeeked = queueSession.peekNext();
      REQUIRE(optPeeked);

      queueSession.next();
      CHECK(queueSession.nowPlayingTrackId() == optPeeked);
    }

    SECTION("direct repeat mode change re-prepares pending next")
    {
      auto const tracks = std::vector{track1, track2};
      REQUIRE(queueSession.playQueue(tracks, track2, ListId{10}));
      REQUIRE_FALSE(queueSession.hasNext());
      REQUIRE_FALSE(queueSession.peekNext());

      playbackService.setRepeatMode(rt::RepeatMode::All);

      CHECK(queueSession.hasNext());
      CHECK(queueSession.peekNext() == track1);
    }

    SECTION("direct shuffle mode change re-prepares pending next")
    {
      auto const tracks = std::vector{track1, track2};
      REQUIRE(queueSession.playQueue(tracks, track2, ListId{10}));
      REQUIRE_FALSE(queueSession.hasNext());
      REQUIRE_FALSE(queueSession.peekNext());

      playbackService.setShuffleMode(rt::ShuffleMode::On);

      CHECK(queueSession.hasNext());
      CHECK(queueSession.peekNext() == track1);
    }

    SECTION("restoreQueue builds an idle queue at the saved track and position")
    {
      auto const tracks = std::vector{track1, track2, track3};
      auto const session = rt::PlaybackSessionState{
        .sourceListId = ListId{10},
        .trackId = track2,
        .positionMs = 4321,
        .shuffleMode = rt::ShuffleMode::On,
        .repeatMode = rt::RepeatMode::All,
      };

      REQUIRE(queueSession.restoreQueue(tracks, session, ListId{10}));

      CHECK(queueSession.isActive());
      CHECK(queueSession.nowPlayingTrackId() == track2);
      CHECK(queueSession.sourceListId() == ListId{10});
      CHECK(playbackService.state().transport == audio::Transport::Idle);
      CHECK(playbackService.state().nowPlaying.trackId == track2);
      CHECK(playbackService.state().elapsed == std::chrono::milliseconds{4321});
      CHECK(playbackService.state().mode.shuffle == rt::ShuffleMode::On);
      CHECK(playbackService.state().mode.repeat == rt::RepeatMode::All);
    }

    SECTION("restoreQueue falls back to the queue head when the saved track was evicted")
    {
      auto const tracks = std::vector{track1, track2, track3};
      auto const session = rt::PlaybackSessionState{
        .sourceListId = ListId{10},
        .trackId = missingTrack,
        .positionMs = 4321,
      };

      REQUIRE(queueSession.restoreQueue(tracks, session, ListId{10}));

      CHECK(queueSession.nowPlayingTrackId() == track1);
      CHECK(playbackService.state().nowPlaying.trackId == track1);
      CHECK(playbackService.state().elapsed == std::chrono::milliseconds{0});
    }

    SECTION("restoreQueue falls back to the queue head when the saved track no longer resolves")
    {
      auto const tracks = std::vector{track1, missingTrack, track3};
      auto const session = rt::PlaybackSessionState{
        .sourceListId = ListId{10},
        .trackId = missingTrack,
        .positionMs = 4321,
      };

      REQUIRE(queueSession.restoreQueue(tracks, session, ListId{10}));

      CHECK(queueSession.nowPlayingTrackId() == track1);
      CHECK(playbackService.state().nowPlaying.trackId == track1);
      CHECK(playbackService.state().elapsed == std::chrono::milliseconds{0});
    }

    SECTION("restoreQueue preserves the active queue when restore cannot resolve any candidate")
    {
      auto const tracks = std::vector{track1, track2, track3};
      queueSession.playQueue(tracks, track2, ListId{10});
      REQUIRE(queueSession.nowPlayingTrackId() == track2);

      auto const session = rt::PlaybackSessionState{
        .sourceListId = ListId{11},
        .trackId = missingTrack,
        .positionMs = 4321,
      };

      CHECK_FALSE(queueSession.restoreQueue({missingTrack}, session, ListId{11}));

      CHECK(queueSession.isActive());
      CHECK(queueSession.nowPlayingTrackId() == track2);
      CHECK(queueSession.sourceListId() == ListId{10});
      CHECK(playbackService.state().nowPlaying.trackId == track2);
    }
  }

  TEST_CASE("PlaybackQueueSession - restored queue resumes playback on explicit play",
            "[uimodel][unit][playback-queue][session]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};
    auto const flacPath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const firstTrack = fixture.testLib.addTrack({.title = "First", .uri = flacPath});
    auto const restoredTrack = fixture.testLib.addTrack({.title = "Restored", .uri = flacPath});
    auto const session = rt::PlaybackSessionState{
      .sourceListId = ListId{10},
      .trackId = restoredTrack,
      .positionMs = 50,
    };

    auto queueSession = PlaybackQueueSession{fixture.playbackService, fixture.notificationService};
    REQUIRE(queueSession.restoreQueue({firstTrack, restoredTrack}, session, ListId{10}));

    CHECK(queueSession.isActive());
    CHECK(queueSession.nowPlayingTrackId() == restoredTrack);
    CHECK(fixture.playbackService.state().transport == audio::Transport::Idle);
    CHECK(fixture.renderTarget == nullptr);

    fixture.onDevicesChangedCb(fixture.status.devices);
    CHECK(fixture.renderTarget == nullptr);

    queueSession.resume();

    CHECK(fixture.renderTarget != nullptr);
    CHECK(fixture.playbackService.state().transport == audio::Transport::Playing);
    CHECK(fixture.playbackService.state().nowPlaying.trackId == restoredTrack);
    CHECK(fixture.playbackService.state().elapsed == std::chrono::milliseconds{50});
  }

  TEST_CASE("PlaybackQueueSession - non-gapless prepared successor advances through idle fallback",
            "[uimodel][unit][playback-queue][gapless]")
  {
    auto fixture = PlaybackFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const flacPath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const mp3Path = audio::test::requireAudioFixture("basic_metadata.mp3").string();
    auto const currentTrack = fixture.testLib.addTrack({.title = "Current FLAC", .uri = flacPath});
    auto const nextTrack = fixture.testLib.addTrack({.title = "Fallback MP3", .uri = mp3Path});

    auto queueSession = PlaybackQueueSession{fixture.playbackService, fixture.notificationService};
    REQUIRE(queueSession.playQueue({currentTrack, nextTrack}, currentTrack, ListId{10}));
    REQUIRE(queueSession.nowPlayingTrackId() == currentTrack);
    REQUIRE(fixture.renderTarget != nullptr);

    auto buffer = std::array<std::byte, 4096>{};
    bool isDrained = false;

    for (std::int32_t i = 0; i < 100000 && !isDrained; ++i)
    {
      isDrained = fixture.renderTarget->renderPcm(buffer).drained;
      fixture.executor.drain();
    }

    REQUIRE(isDrained);
    fixture.renderTarget->onDrainComplete();

    for (std::int32_t i = 0; i < 100000 && queueSession.nowPlayingTrackId() != nextTrack; ++i)
    {
      fixture.executor.drain();
    }

    REQUIRE(queueSession.nowPlayingTrackId() == nextTrack);
    CHECK(fixture.playbackService.state().nowPlaying.trackId == nextTrack);
    CHECK(fixture.playbackService.state().nowPlaying.title == "Fallback MP3");
    CHECK(queueSession.isActive());
  }

  TEST_CASE("PlaybackQueueSession - recoverable playback failure skips to next track",
            "[uimodel][unit][playback-queue][error]")
  {
    auto fixture = PlaybackFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const flacPath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const brokenTrack = fixture.testLib.addTrack({.title = "Broken Track", .uri = "broken.txt"});
    auto const playableTrack = fixture.testLib.addTrack({.title = "Playable Track", .uri = flacPath});

    auto queueSession = PlaybackQueueSession{fixture.playbackService, fixture.notificationService};
    REQUIRE(queueSession.playQueue({brokenTrack, playableTrack}, brokenTrack, ListId{10}));

    REQUIRE(fixture.executor.drainUntil([&] { return queueSession.nowPlayingTrackId() == playableTrack; }));

    CHECK(queueSession.isActive());
    CHECK(fixture.playbackService.state().nowPlaying.trackId == playableTrack);
    CHECK(fixture.playbackService.state().nowPlaying.title == "Playable Track");
    CHECK(hasNotification(fixture.notificationService, NotificationSeverity::Warning, "Skipped 1 unplayable track"));
    CHECK(notificationCount(fixture.notificationService, NotificationSeverity::Error, "Could not play") == 0);
  }

  TEST_CASE("PlaybackQueueSession - repeated recoverable playback failures stop the queue",
            "[uimodel][unit][playback-queue][error]")
  {
    auto fixture = PlaybackFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const broken1 = fixture.testLib.addTrack({.title = "Broken 1", .uri = "broken1.txt"});
    auto const broken2 = fixture.testLib.addTrack({.title = "Broken 2", .uri = "broken2.txt"});
    auto const broken3 = fixture.testLib.addTrack({.title = "Broken 3", .uri = "broken3.txt"});

    auto queueSession = PlaybackQueueSession{fixture.playbackService, fixture.notificationService};
    REQUIRE(queueSession.playQueue({broken1, broken2, broken3}, broken1, ListId{10}));

    REQUIRE(fixture.executor.drainUntil([&] { return !queueSession.isActive(); }));

    CHECK_FALSE(queueSession.nowPlayingTrackId());
    CHECK(fixture.playbackService.state().nowPlaying.trackId == kInvalidTrackId);
    CHECK(hasNotification(fixture.notificationService, NotificationSeverity::Warning, "Skipped 2 unplayable tracks"));
    CHECK(notificationCount(fixture.notificationService, NotificationSeverity::Error, "Could not play") == 0);
    CHECK(hasNotification(
      fixture.notificationService, NotificationSeverity::Error, "Playback stopped after 3 unplayable tracks"));
  }

  TEST_CASE("PlaybackQueueSession - explicit skip resets recoverable failure streak",
            "[uimodel][unit][playback-queue][error]")
  {
    auto fixture = PlaybackFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const flacPath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const broken1 = fixture.testLib.addTrack({.title = "Broken 1", .uri = "broken1.txt"});
    auto const playable1 = fixture.testLib.addTrack({.title = "Playable 1", .uri = flacPath});
    auto const broken2 = fixture.testLib.addTrack({.title = "Broken 2", .uri = "broken2.txt"});
    auto const broken3 = fixture.testLib.addTrack({.title = "Broken 3", .uri = "broken3.txt"});
    auto const playable2 = fixture.testLib.addTrack({.title = "Playable 2", .uri = flacPath});

    auto queueSession = PlaybackQueueSession{fixture.playbackService, fixture.notificationService};
    REQUIRE(queueSession.playQueue({broken1, playable1, broken2, broken3, playable2}, broken1, ListId{10}));

    REQUIRE(fixture.executor.drainUntil([&] { return queueSession.nowPlayingTrackId() == playable1; }));

    queueSession.next();

    REQUIRE(fixture.executor.drainUntil([&] { return queueSession.nowPlayingTrackId() == playable2; }));

    CHECK(queueSession.isActive());
    CHECK_FALSE(hasNotification(
      fixture.notificationService, NotificationSeverity::Error, "Playback stopped after 3 unplayable tracks"));
  }

  TEST_CASE("PlaybackQueueSession - global synchronous playback failure stops without scanning queue",
            "[uimodel][unit][playback-queue][error]")
  {
    auto fixture = PlaybackFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const flacPath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const track1 = fixture.testLib.addTrack({.title = "Track 1", .uri = flacPath});
    auto const track2 = fixture.testLib.addTrack({.title = "Track 2", .uri = flacPath});
    auto const track3 = fixture.testLib.addTrack({.title = "Track 3", .uri = flacPath});

    auto queueSession = PlaybackQueueSession{fixture.playbackService, fixture.notificationService};
    REQUIRE(queueSession.playQueue({track1, track2, track3}, track1, ListId{10}));

    fixture.onDevicesChangedCb({});
    fixture.executor.drain();
    fixture.playbackService.setOutputDevice(
      audio::BackendId{"mock_backend"}, audio::DeviceId{"missing_device"}, audio::ProfileId{audio::kProfileShared});
    REQUIRE_FALSE(fixture.playbackService.state().ready);

    queueSession.next();

    CHECK_FALSE(queueSession.isActive());
    CHECK(notificationCount(fixture.notificationService, NotificationSeverity::Error, "Could not start playback") == 1);
  }

  TEST_CASE("PlaybackQueueSession - device playback failure stops the queue", "[uimodel][unit][playback-queue][error]")
  {
    auto fixture = PlaybackFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const flacPath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const currentTrack = fixture.testLib.addTrack({.title = "Current Track", .uri = flacPath});
    auto const nextTrack = fixture.testLib.addTrack({.title = "Next Track", .uri = flacPath});

    auto queueSession = PlaybackQueueSession{fixture.playbackService, fixture.notificationService};
    REQUIRE(queueSession.playQueue({currentTrack, nextTrack}, currentTrack, ListId{10}));
    REQUIRE(fixture.renderTarget != nullptr);

    fixture.renderTarget->onBackendError("device lost");

    REQUIRE(fixture.executor.drainUntil([&] { return !queueSession.isActive(); }));

    CHECK_FALSE(queueSession.nowPlayingTrackId());
    CHECK(fixture.playbackService.state().nowPlaying.trackId == kInvalidTrackId);
    CHECK(hasNotification(fixture.notificationService, NotificationSeverity::Error, "Playback device failed"));
  }
} // namespace ao::uimodel::test
