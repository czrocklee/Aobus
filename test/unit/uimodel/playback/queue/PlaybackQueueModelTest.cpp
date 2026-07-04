// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/audio/AudioFixtureUtils.h"
#include "test/unit/runtime/PlaybackServiceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/Backend.h>
#include <ao/audio/IRenderTarget.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/source/ListSourceStore.h>
#include <ao/uimodel/playback/queue/PlaybackQueueModel.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
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

  TEST_CASE("PlaybackQueueModel - basic controls", "[uimodel][unit][playback]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto changes = LibraryChanges{};
    auto listSourceStore = ListSourceStore{testLib.library(), changes};
    auto viewService = ViewService{executor, testLib.library(), listSourceStore};
    auto notificationService = NotificationService{};

    auto playbackService = PlaybackService{executor, viewService, testLib.library(), notificationService};
    addReadyAudioProvider(playbackService);

    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const track1 = testLib.addTrack({.title = "Track 1", .uri = fixturePath});
    auto const track2 = testLib.addTrack({.title = "Track 2", .uri = fixturePath});
    auto const track3 = testLib.addTrack({.title = "Track 3", .uri = fixturePath});
    auto const missingTrack = TrackId{999};

    auto queueModel = PlaybackQueueModel{playbackService, notificationService};

    SECTION("initial state is inactive")
    {
      CHECK(queueModel.isActive() == false);
      CHECK(queueModel.nowPlayingTrackId() == std::nullopt);
      CHECK(queueModel.sourceListId() == kInvalidListId);
    }

    SECTION("resume handles inactive sequence")
    {
      queueModel.resume(); // Should not crash
    }

    SECTION("transport actions when inactive")
    {
      queueModel.setShuffleMode(rt::ShuffleMode::On);
      queueModel.setRepeatMode(rt::RepeatMode::One);
      queueModel.next();
      queueModel.previous();
      // Should not crash or activate
      CHECK(queueModel.isActive() == false);
    }

    SECTION("playQueue fails if startTrackId is not in the list")
    {
      auto const tracks = std::vector{track1, track2};
      auto const result = queueModel.playQueue(tracks, track3, ListId{10});
      CHECK(result == false);
      CHECK(queueModel.isActive() == false);
    }

    SECTION("playQueue sets up the queue correctly")
    {
      auto const tracks = std::vector{track1, track2, track3};
      auto const result = queueModel.playQueue(tracks, track2, ListId{10});

      CHECK(result == true);
      CHECK(queueModel.isActive() == true);
      CHECK(queueModel.nowPlayingTrackId() == track2);
      CHECK(queueModel.sourceListId() == ListId{10});
      CHECK(playbackService.state().trackId == track2);
    }

    SECTION("playQueue fails on empty list")
    {
      auto const tracks = std::vector<TrackId>{};
      auto const result = queueModel.playQueue(tracks, track2, ListId{10});
      CHECK(result == false);
    }

    SECTION("playQueue fails on missing track")
    {
      auto const tracks = std::vector{missingTrack};
      auto const result = queueModel.playQueue(tracks, missingTrack, ListId{10});
      CHECK(result == false);
    }

    SECTION("advanceToNext when active and idle")
    {
      auto const tracks = std::vector{track1, track2, track3};
      queueModel.playQueue(tracks, track1, ListId{10});
      CHECK(queueModel.nowPlayingTrackId() == track1);

      queueModel.next();
      CHECK(queueModel.nowPlayingTrackId() == track2);

      queueModel.next();
      CHECK(queueModel.nowPlayingTrackId() == track3);

      // End of list, no repeat
      queueModel.next();
      CHECK(queueModel.isActive() == false);
    }

    SECTION("peekNext does not move the current cursor")
    {
      auto const tracks = std::vector{track1, track2, track3};
      queueModel.playQueue(tracks, track1, ListId{10});

      auto const optFirstPeek = queueModel.peekNext();
      auto const optSecondPeek = queueModel.peekNext();

      REQUIRE(optFirstPeek);
      CHECK(*optFirstPeek == track2);
      CHECK(optSecondPeek == optFirstPeek);
      CHECK(queueModel.nowPlayingTrackId() == track1);
      CHECK(playbackService.state().trackId == track1);
    }

    SECTION("now-playing change commits pending successor")
    {
      auto const tracks = std::vector{track1, track2, track3};
      queueModel.playQueue(tracks, track1, ListId{10});

      REQUIRE(queueModel.peekNext() == track2);
      REQUIRE(playbackService.playTrack(track2, ListId{10}));

      CHECK(queueModel.nowPlayingTrackId() == track2);
      CHECK(queueModel.peekNext() == track3);
    }

    SECTION("now-playing change from another source list does not commit pending successor")
    {
      auto const tracks = std::vector{track1, track2, track3};
      queueModel.playQueue(tracks, track1, ListId{10});

      REQUIRE(queueModel.peekNext() == track2);
      REQUIRE(playbackService.playTrack(track2, ListId{99}));

      CHECK(queueModel.nowPlayingTrackId() == track1);
      CHECK(queueModel.peekNext() == track2);
      CHECK(queueModel.sourceListId() == ListId{10});
    }

    SECTION("previous when active")
    {
      auto const tracks = std::vector{track1, track2, track3};
      queueModel.playQueue(tracks, track2, ListId{10});
      CHECK(queueModel.nowPlayingTrackId() == track2);

      queueModel.previous();
      CHECK(queueModel.nowPlayingTrackId() == track1);

      // At start of list, no repeat
      queueModel.previous();
      CHECK(queueModel.nowPlayingTrackId() == track1); // doesn't wrap around
    }

    SECTION("Repeat One")
    {
      queueModel.setRepeatMode(rt::RepeatMode::One);
      auto const tracks1 = std::vector{track1, track2};
      queueModel.playQueue(tracks1, track1, ListId{10});

      queueModel.next();
      CHECK(queueModel.nowPlayingTrackId() == track1);
    }

    SECTION("Repeat All")
    {
      queueModel.setRepeatMode(rt::RepeatMode::All);
      auto const tracks2 = std::vector{track1, track2};
      queueModel.playQueue(tracks2, track2, ListId{10});

      queueModel.next();
      CHECK(queueModel.nowPlayingTrackId() == track1);

      queueModel.previous();
      CHECK(queueModel.nowPlayingTrackId() == track2);
    }

    SECTION("Previous at start with Repeat All wraps to end")
    {
      queueModel.setRepeatMode(rt::RepeatMode::All);
      auto const tracks = std::vector{track1, track2, track3};
      queueModel.playQueue(tracks, track1, ListId{10});
      CHECK(queueModel.nowPlayingTrackId() == track1);

      queueModel.previous();
      CHECK(queueModel.nowPlayingTrackId() == track3);
    }

    SECTION("Previous skips invalid tracks")
    {
      // Setup: [1, 999, 3] where 999 is invalid
      auto const tracks = std::vector{track1, missingTrack, track3};
      queueModel.playQueue(tracks, track3, ListId{10});
      CHECK(queueModel.nowPlayingTrackId() == track3);

      // From 3, go previous. Should skip 999 and find 1.
      queueModel.previous();
      CHECK(queueModel.nowPlayingTrackId() == track1);
    }

    SECTION("Previous at start with Repeat All skips invalid tracks at end")
    {
      queueModel.setRepeatMode(rt::RepeatMode::All);
      // Setup: [1, 2, 999] where 999 is invalid
      auto const tracks = std::vector{track1, track2, missingTrack};
      queueModel.playQueue(tracks, track1, ListId{10});
      CHECK(queueModel.nowPlayingTrackId() == track1);

      // From 1, go previous. Should wrap to end, skip 999, and find 2.
      queueModel.previous();
      CHECK(queueModel.nowPlayingTrackId() == track2);
    }

    SECTION("Shuffle mode next picks random track")
    {
      queueModel.setShuffleMode(rt::ShuffleMode::On);
      auto const tracks = std::vector{track1, track2, track3};
      queueModel.playQueue(tracks, track1, ListId{10});

      queueModel.next();
      CHECK(queueModel.isActive() == true);
    }

    SECTION("Shuffle mode next commits the optPeeked successor")
    {
      queueModel.setShuffleMode(rt::ShuffleMode::On);
      auto const tracks = std::vector{track1, track2, track3};
      queueModel.playQueue(tracks, track1, ListId{10});

      auto const optPeeked = queueModel.peekNext();
      REQUIRE(optPeeked);

      queueModel.next();
      CHECK(queueModel.nowPlayingTrackId() == optPeeked);
    }
  }

  TEST_CASE("PlaybackQueueModel - non-gapless prepared successor advances through idle fallback",
            "[uimodel][unit][playback][queue][gapless]")
  {
    auto fixture = PlaybackFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const flacPath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const mp3Path = audio::test::requireAudioFixture("basic_metadata.mp3").string();
    auto const currentTrack = fixture.testLib.addTrack({.title = "Current FLAC", .uri = flacPath});
    auto const nextTrack = fixture.testLib.addTrack({.title = "Fallback MP3", .uri = mp3Path});

    auto queueModel = PlaybackQueueModel{fixture.playbackService, fixture.notificationService};
    REQUIRE(queueModel.playQueue({currentTrack, nextTrack}, currentTrack, ListId{10}));
    REQUIRE(queueModel.nowPlayingTrackId() == currentTrack);
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

    for (std::int32_t i = 0; i < 100000 && queueModel.nowPlayingTrackId() != nextTrack; ++i)
    {
      fixture.executor.drain();
    }

    REQUIRE(queueModel.nowPlayingTrackId() == nextTrack);
    CHECK(fixture.playbackService.state().trackId == nextTrack);
    CHECK(fixture.playbackService.state().trackTitle == "Fallback MP3");
    CHECK(queueModel.isActive());
  }

  TEST_CASE("PlaybackQueueModel - recoverable playback failure skips to next track",
            "[uimodel][unit][playback][queue][error]")
  {
    auto fixture = PlaybackFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const flacPath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const brokenTrack = fixture.testLib.addTrack({.title = "Broken Track", .uri = "broken.txt"});
    auto const playableTrack = fixture.testLib.addTrack({.title = "Playable Track", .uri = flacPath});

    auto queueModel = PlaybackQueueModel{fixture.playbackService, fixture.notificationService};
    REQUIRE(queueModel.playQueue({brokenTrack, playableTrack}, brokenTrack, ListId{10}));

    REQUIRE(fixture.executor.drainUntil([&] { return queueModel.nowPlayingTrackId() == playableTrack; }));

    CHECK(queueModel.isActive());
    CHECK(fixture.playbackService.state().trackId == playableTrack);
    CHECK(fixture.playbackService.state().trackTitle == "Playable Track");
    CHECK(hasNotification(fixture.notificationService, NotificationSeverity::Warning, "Skipped 1 unplayable track"));
    CHECK(notificationCount(fixture.notificationService, NotificationSeverity::Error, "Could not play") == 0);
  }

  TEST_CASE("PlaybackQueueModel - repeated recoverable playback failures stop the queue",
            "[uimodel][unit][playback][queue][error]")
  {
    auto fixture = PlaybackFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const broken1 = fixture.testLib.addTrack({.title = "Broken 1", .uri = "broken1.txt"});
    auto const broken2 = fixture.testLib.addTrack({.title = "Broken 2", .uri = "broken2.txt"});
    auto const broken3 = fixture.testLib.addTrack({.title = "Broken 3", .uri = "broken3.txt"});

    auto queueModel = PlaybackQueueModel{fixture.playbackService, fixture.notificationService};
    REQUIRE(queueModel.playQueue({broken1, broken2, broken3}, broken1, ListId{10}));

    REQUIRE(fixture.executor.drainUntil([&] { return !queueModel.isActive(); }));

    CHECK_FALSE(queueModel.nowPlayingTrackId());
    CHECK(fixture.playbackService.state().trackId == kInvalidTrackId);
    CHECK(hasNotification(fixture.notificationService, NotificationSeverity::Warning, "Skipped 2 unplayable tracks"));
    CHECK(notificationCount(fixture.notificationService, NotificationSeverity::Error, "Could not play") == 0);
    CHECK(hasNotification(
      fixture.notificationService, NotificationSeverity::Error, "Playback stopped after 3 unplayable tracks"));
  }

  TEST_CASE("PlaybackQueueModel - explicit skip resets recoverable failure streak",
            "[uimodel][unit][playback][queue][error]")
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

    auto queueModel = PlaybackQueueModel{fixture.playbackService, fixture.notificationService};
    REQUIRE(queueModel.playQueue({broken1, playable1, broken2, broken3, playable2}, broken1, ListId{10}));

    REQUIRE(fixture.executor.drainUntil([&] { return queueModel.nowPlayingTrackId() == playable1; }));

    queueModel.next();

    REQUIRE(fixture.executor.drainUntil([&] { return queueModel.nowPlayingTrackId() == playable2; }));

    CHECK(queueModel.isActive());
    CHECK_FALSE(hasNotification(
      fixture.notificationService, NotificationSeverity::Error, "Playback stopped after 3 unplayable tracks"));
  }

  TEST_CASE("PlaybackQueueModel - global synchronous playback failure stops without scanning queue",
            "[uimodel][unit][playback][queue][error]")
  {
    auto fixture = PlaybackFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const flacPath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const track1 = fixture.testLib.addTrack({.title = "Track 1", .uri = flacPath});
    auto const track2 = fixture.testLib.addTrack({.title = "Track 2", .uri = flacPath});
    auto const track3 = fixture.testLib.addTrack({.title = "Track 3", .uri = flacPath});

    auto queueModel = PlaybackQueueModel{fixture.playbackService, fixture.notificationService};
    REQUIRE(queueModel.playQueue({track1, track2, track3}, track1, ListId{10}));

    fixture.onDevicesChangedCb({});
    fixture.executor.drain();
    fixture.playbackService.setOutputDevice(
      audio::BackendId{"mock_backend"}, audio::DeviceId{"missing_device"}, audio::ProfileId{audio::kProfileShared});
    REQUIRE_FALSE(fixture.playbackService.state().ready);

    queueModel.next();

    CHECK_FALSE(queueModel.isActive());
    CHECK(notificationCount(fixture.notificationService, NotificationSeverity::Error, "Could not start playback") == 1);
  }

  TEST_CASE("PlaybackQueueModel - device playback failure stops the queue", "[uimodel][unit][playback][queue][error]")
  {
    auto fixture = PlaybackFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const flacPath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const currentTrack = fixture.testLib.addTrack({.title = "Current Track", .uri = flacPath});
    auto const nextTrack = fixture.testLib.addTrack({.title = "Next Track", .uri = flacPath});

    auto queueModel = PlaybackQueueModel{fixture.playbackService, fixture.notificationService};
    REQUIRE(queueModel.playQueue({currentTrack, nextTrack}, currentTrack, ListId{10}));
    REQUIRE(fixture.renderTarget != nullptr);

    fixture.renderTarget->onBackendError("device lost");

    REQUIRE(fixture.executor.drainUntil([&] { return !queueModel.isActive(); }));

    CHECK_FALSE(queueModel.nowPlayingTrackId());
    CHECK(fixture.playbackService.state().trackId == kInvalidTrackId);
    CHECK(hasNotification(fixture.notificationService, NotificationSeverity::Error, "Playback device failed"));
  }
} // namespace ao::uimodel::test
