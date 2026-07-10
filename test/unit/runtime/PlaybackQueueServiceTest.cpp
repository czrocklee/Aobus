// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/runtime/PlaybackServiceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/RenderTarget.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackFailure.h>
#include <ao/rt/PlaybackQueueService.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    std::size_t notificationCount(NotificationService const& notifications,
                                  NotificationSeverity severity,
                                  std::string_view messageFragment)
    {
      std::size_t count = 0;

      for (auto const& entry : notifications.feed().entries)
      {
        if (entry.severity == severity && entry.message.contains(messageFragment))
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

    std::optional<TrackId> currentQueueTrack(PlaybackQueueService const& queue)
    {
      auto const& state = queue.state();

      if (!state.optCurrentIndex || *state.optCurrentIndex >= state.trackIds.size())
      {
        return std::nullopt;
      }

      return state.trackIds[*state.optCurrentIndex];
    }

    std::optional<TrackId> pendingQueueTrack(PlaybackQueueService const& queue)
    {
      auto const& state = queue.state();

      if (!state.optPendingNextIndex || *state.optPendingNextIndex >= state.trackIds.size())
      {
        return std::nullopt;
      }

      return state.trackIds[*state.optPendingNextIndex];
    }
  } // namespace

  TEST_CASE("PlaybackQueueService - basic controls", "[runtime][unit][playback]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = MockExecutor{};
    auto changes = LibraryChanges{};
    auto trackSourceCache = TrackSourceCache{libraryFixture.library(), changes};
    auto viewService = ViewService{executor, libraryFixture.library(), trackSourceCache};
    auto notificationService = NotificationService{};

    auto playbackService = PlaybackService{executor, viewService, libraryFixture.library(), notificationService};
    addReadyAudioProvider(playbackService);

    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const track1 = libraryFixture.addTrack({.title = "Track 1", .uri = fixturePath});
    auto const track2 = libraryFixture.addTrack({.title = "Track 2", .uri = fixturePath});
    auto const track3 = libraryFixture.addTrack({.title = "Track 3", .uri = fixturePath});
    auto const missingTrack = TrackId{999};

    auto queueSession = PlaybackQueueService{executor, playbackService, notificationService};

    SECTION("initial state is inactive")
    {
      CHECK_FALSE(queueSession.state().optCurrentIndex);
      CHECK(currentQueueTrack(queueSession) == std::nullopt);
      CHECK(queueSession.state().sourceListId == kInvalidListId);
    }

    SECTION("resume handles inactive sequence")
    {
      queueSession.resume(); // Should not crash
    }

    SECTION("transport actions when inactive")
    {
      playbackService.setShuffleMode(ShuffleMode::On);
      playbackService.setRepeatMode(RepeatMode::One);
      queueSession.next();
      queueSession.previous();
      // Should not crash or activate
      CHECK_FALSE(queueSession.state().optCurrentIndex);
    }

    SECTION("playQueue fails if startTrackId is not in the list")
    {
      auto const tracks = std::vector{track1, track2};
      auto const result = queueSession.playQueue(tracks, track3, ListId{10});
      CHECK_FALSE(result);
      CHECK_FALSE(queueSession.state().optCurrentIndex);
    }

    SECTION("playQueue sets up the queue correctly")
    {
      auto const tracks = std::vector{track1, track2, track3};
      auto const result = queueSession.playQueue(tracks, track2, ListId{10});

      CHECK(result);
      CHECK(queueSession.state().optCurrentIndex);
      CHECK(currentQueueTrack(queueSession) == track2);
      CHECK(queueSession.state().sourceListId == ListId{10});
      CHECK(playbackService.state().nowPlaying.trackId == track2);
    }

    SECTION("playQueue fails on empty list")
    {
      auto const tracks = std::vector<TrackId>{};
      auto const result = queueSession.playQueue(tracks, track2, ListId{10});
      CHECK_FALSE(result);
    }

    SECTION("playQueue fails on missing track")
    {
      auto const tracks = std::vector{missingTrack};
      auto const result = queueSession.playQueue(tracks, missingTrack, ListId{10});
      CHECK_FALSE(result);
    }

    SECTION("advanceToNext when active and idle")
    {
      auto const tracks = std::vector{track1, track2, track3};
      REQUIRE(queueSession.playQueue(tracks, track1, ListId{10}));
      CHECK(currentQueueTrack(queueSession) == track1);

      queueSession.next();
      CHECK(currentQueueTrack(queueSession) == track2);

      queueSession.next();
      CHECK(currentQueueTrack(queueSession) == track3);

      // End of list, no repeat
      queueSession.next();
      CHECK_FALSE(queueSession.state().optCurrentIndex);
    }

    SECTION("peekNext does not move the current cursor")
    {
      auto const tracks = std::vector{track1, track2, track3};
      REQUIRE(queueSession.playQueue(tracks, track1, ListId{10}));

      auto const optFirstPeek = pendingQueueTrack(queueSession);
      auto const optSecondPeek = pendingQueueTrack(queueSession);

      REQUIRE(optFirstPeek);
      CHECK(*optFirstPeek == track2);
      CHECK(optSecondPeek == optFirstPeek);
      CHECK(currentQueueTrack(queueSession) == track1);
      CHECK(playbackService.state().nowPlaying.trackId == track1);
    }

    SECTION("now-playing change commits pending successor")
    {
      auto const tracks = std::vector{track1, track2, track3};
      REQUIRE(queueSession.playQueue(tracks, track1, ListId{10}));

      REQUIRE(pendingQueueTrack(queueSession) == track2);
      REQUIRE(playbackService.playTrack(track2, ListId{10}));

      CHECK(currentQueueTrack(queueSession) == track2);
      CHECK(pendingQueueTrack(queueSession) == track3);
    }

    SECTION("now-playing change from another source list does not commit pending successor")
    {
      auto const tracks = std::vector{track1, track2, track3};
      REQUIRE(queueSession.playQueue(tracks, track1, ListId{10}));

      REQUIRE(pendingQueueTrack(queueSession) == track2);
      REQUIRE(playbackService.playTrack(track2, ListId{99}));

      CHECK(currentQueueTrack(queueSession) == track1);
      CHECK(pendingQueueTrack(queueSession) == track2);
      CHECK(queueSession.state().sourceListId == ListId{10});
    }

    SECTION("previous when active")
    {
      auto const tracks = std::vector{track1, track2, track3};
      REQUIRE(queueSession.playQueue(tracks, track2, ListId{10}));
      CHECK(currentQueueTrack(queueSession) == track2);

      queueSession.previous();
      CHECK(currentQueueTrack(queueSession) == track1);

      // At start of list, no repeat
      queueSession.previous();
      CHECK(currentQueueTrack(queueSession) == track1); // doesn't wrap around
    }

    SECTION("Repeat One")
    {
      playbackService.setRepeatMode(RepeatMode::One);
      auto const tracks1 = std::vector{track1, track2};
      REQUIRE(queueSession.playQueue(tracks1, track1, ListId{10}));

      queueSession.next();
      CHECK(currentQueueTrack(queueSession) == track1);
    }

    SECTION("Repeat All")
    {
      playbackService.setRepeatMode(RepeatMode::All);
      auto const tracks2 = std::vector{track1, track2};
      REQUIRE(queueSession.playQueue(tracks2, track2, ListId{10}));

      queueSession.next();
      CHECK(currentQueueTrack(queueSession) == track1);

      queueSession.previous();
      CHECK(currentQueueTrack(queueSession) == track2);
    }

    SECTION("Previous at start with Repeat All wraps to end")
    {
      playbackService.setRepeatMode(RepeatMode::All);
      auto const tracks = std::vector{track1, track2, track3};
      REQUIRE(queueSession.playQueue(tracks, track1, ListId{10}));
      CHECK(currentQueueTrack(queueSession) == track1);

      queueSession.previous();
      CHECK(currentQueueTrack(queueSession) == track3);
    }

    SECTION("Previous skips invalid tracks")
    {
      // Setup: [1, 999, 3] where 999 is invalid
      auto const tracks = std::vector{track1, missingTrack, track3};
      REQUIRE(queueSession.playQueue(tracks, track3, ListId{10}));
      CHECK(currentQueueTrack(queueSession) == track3);

      // From 3, go previous. Should skip 999 and find 1.
      queueSession.previous();
      CHECK(currentQueueTrack(queueSession) == track1);
    }

    SECTION("Previous at start with Repeat All skips invalid tracks at end")
    {
      playbackService.setRepeatMode(RepeatMode::All);
      // Setup: [1, 2, 999] where 999 is invalid
      auto const tracks = std::vector{track1, track2, missingTrack};
      REQUIRE(queueSession.playQueue(tracks, track1, ListId{10}));
      CHECK(currentQueueTrack(queueSession) == track1);

      // From 1, go previous. Should wrap to end, skip 999, and find 2.
      queueSession.previous();
      CHECK(currentQueueTrack(queueSession) == track2);
    }

    SECTION("Shuffle mode next picks random track")
    {
      playbackService.setShuffleMode(ShuffleMode::On);
      auto const tracks = std::vector{track1, track2, track3};
      REQUIRE(queueSession.playQueue(tracks, track1, ListId{10}));

      queueSession.next();
      CHECK(queueSession.state().optCurrentIndex);
    }

    SECTION("Shuffle mode next commits the optPeeked successor")
    {
      playbackService.setShuffleMode(ShuffleMode::On);
      auto const tracks = std::vector{track1, track2, track3};
      REQUIRE(queueSession.playQueue(tracks, track1, ListId{10}));

      auto const optPeeked = pendingQueueTrack(queueSession);
      REQUIRE(optPeeked);

      queueSession.next();
      CHECK(currentQueueTrack(queueSession) == optPeeked);
    }

    SECTION("direct repeat mode change re-prepares pending next")
    {
      auto const tracks = std::vector{track1, track2};
      REQUIRE(queueSession.playQueue(tracks, track2, ListId{10}));
      REQUIRE_FALSE(queueSession.hasNext());
      REQUIRE_FALSE(pendingQueueTrack(queueSession));

      playbackService.setRepeatMode(RepeatMode::All);

      CHECK(queueSession.hasNext());
      CHECK(pendingQueueTrack(queueSession) == track1);
    }

    SECTION("direct shuffle mode change re-prepares pending next")
    {
      auto const tracks = std::vector{track1, track2};
      REQUIRE(queueSession.playQueue(tracks, track2, ListId{10}));
      REQUIRE_FALSE(queueSession.hasNext());
      REQUIRE_FALSE(pendingQueueTrack(queueSession));

      playbackService.setShuffleMode(ShuffleMode::On);

      CHECK(queueSession.hasNext());
      CHECK(pendingQueueTrack(queueSession) == track1);
    }
  }

  TEST_CASE("PlaybackQueueService - publishes current snapshots once per queue mutation",
            "[runtime][unit][playback-queue][events]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const firstTrack = fixture.libraryFixture.addTrack({.title = "First", .uri = fixturePath});
    auto const secondTrack = fixture.libraryFixture.addTrack({.title = "Second", .uri = fixturePath});
    auto queue = PlaybackQueueService{fixture.executor, fixture.playbackService, fixture.notificationService};
    auto revisions = std::vector<std::uint64_t>{};
    auto currentTracks = std::vector<std::optional<TrackId>>{};
    auto sub = queue.onChanged(
      [&](PlaybackQueueState const& event)
      {
        revisions.push_back(event.revision);
        currentTracks.push_back(currentQueueTrack(queue));
        CHECK(event.revision == queue.state().revision);
      });

    auto const missingStart = queue.playQueue({firstTrack, secondTrack}, TrackId{999}, ListId{10});
    REQUIRE_FALSE(missingStart);
    CHECK(revisions.empty());
    CHECK(queue.state().revision == 0);

    REQUIRE(queue.playQueue({firstTrack, secondTrack}, firstTrack, ListId{10}));
    queue.next();
    queue.clear();
    queue.clear();

    CHECK(revisions == std::vector<std::uint64_t>{1, 2, 3});
    REQUIRE(currentTracks.size() == 3);
    CHECK(currentTracks[0] == firstTrack);
    CHECK(currentTracks[1] == secondTrack);
    CHECK_FALSE(currentTracks[2]);
    CHECK(queue.state().revision == 3);
  }

  TEST_CASE("PlaybackQueueService - queued playback callbacks are neutralized after queue destruction",
            "[runtime][unit][playback-queue][lifetime]")
  {
    auto fixture = PlaybackFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();
    auto const brokenTrack = fixture.libraryFixture.addTrack({.title = "Broken", .uri = "broken.txt"});
    auto queuePtr =
      std::make_unique<PlaybackQueueService>(fixture.executor, fixture.playbackService, fixture.notificationService);
    auto changedSub = queuePtr->onChanged([](PlaybackQueueState const&) {});

    REQUIRE(queuePtr->playQueue({brokenTrack}, brokenTrack, ListId{10}));
    queuePtr.reset();
    changedSub.reset();
    REQUIRE(fixture.executor.drainUntil([&] { return !fixture.notificationService.feed().entries.empty(); }));

    auto const feed = fixture.notificationService.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().message.contains("Broken"));
  }

  TEST_CASE("PlaybackQueueService - recovery is decided before public observers and tolerates queue destruction",
            "[runtime][unit][playback-queue][error]")
  {
    auto fixture = PlaybackFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();
    auto const flacPath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const brokenTrack = fixture.libraryFixture.addTrack({.title = "Broken", .uri = "broken.txt"});
    auto const playableTrack = fixture.libraryFixture.addTrack({.title = "Playable", .uri = flacPath});
    auto queuePtr =
      std::make_unique<PlaybackQueueService>(fixture.executor, fixture.playbackService, fixture.notificationService);
    auto optDisposition = std::optional<PlaybackFailureDisposition>{};
    auto failureSub = fixture.playbackService.onPlaybackFailure(
      [&](PlaybackFailure const& failure)
      {
        optDisposition = failure.disposition;
        queuePtr.reset();
      });

    REQUIRE(queuePtr->playQueue({brokenTrack, playableTrack}, brokenTrack, ListId{10}));
    REQUIRE(fixture.executor.drainUntil([&] { return optDisposition.has_value(); }));

    CHECK(*optDisposition == PlaybackFailureDisposition::Recovered);
    CHECK(queuePtr == nullptr);
    CHECK(fixture.playbackService.state().nowPlaying.trackId == playableTrack);
    CHECK(notificationCount(fixture.notificationService, NotificationSeverity::Error, "Could not play") == 0);
  }

  TEST_CASE("PlaybackQueueService - non-gapless prepared successor advances through idle fallback",
            "[runtime][unit][playback-queue][gapless]")
  {
    auto fixture = PlaybackFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const flacPath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const mp3Path = audio::test::requireAudioFixture("basic_metadata.mp3").string();
    auto const currentTrack = fixture.libraryFixture.addTrack({.title = "Current FLAC", .uri = flacPath});
    auto const nextTrack = fixture.libraryFixture.addTrack({.title = "Fallback MP3", .uri = mp3Path});

    auto queueSession = PlaybackQueueService{fixture.executor, fixture.playbackService, fixture.notificationService};
    REQUIRE(queueSession.playQueue({currentTrack, nextTrack}, currentTrack, ListId{10}));
    REQUIRE(currentQueueTrack(queueSession) == currentTrack);
    REQUIRE(fixture.renderTarget != nullptr);

    auto buffer = std::array<std::byte, 4096>{};
    bool isDrained = false;

    for (std::int32_t i = 0; i < 100000 && !isDrained; ++i)
    {
      isDrained = fixture.renderTarget->renderPcm(buffer).drained;
      fixture.executor.drain();
    }

    REQUIRE(isDrained);
    fixture.renderTarget->handleDrainComplete();

    for (std::int32_t i = 0; i < 100000 && currentQueueTrack(queueSession) != nextTrack; ++i)
    {
      fixture.executor.drain();
    }

    REQUIRE(currentQueueTrack(queueSession) == nextTrack);
    CHECK(fixture.playbackService.state().nowPlaying.trackId == nextTrack);
    CHECK(fixture.playbackService.state().nowPlaying.title == "Fallback MP3");
    CHECK(queueSession.state().optCurrentIndex);
  }

  TEST_CASE("PlaybackQueueService - recoverable playback failure skips to next track",
            "[runtime][unit][playback-queue][error]")
  {
    auto fixture = PlaybackFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const flacPath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const brokenTrack = fixture.libraryFixture.addTrack({.title = "Broken Track", .uri = "broken.txt"});
    auto const playableTrack = fixture.libraryFixture.addTrack({.title = "Playable Track", .uri = flacPath});

    auto queueSession = PlaybackQueueService{fixture.executor, fixture.playbackService, fixture.notificationService};
    auto failures = std::vector<PlaybackFailure>{};
    auto failureSub =
      fixture.playbackService.onPlaybackFailure([&](PlaybackFailure const& failure) { failures.push_back(failure); });
    REQUIRE(queueSession.playQueue({brokenTrack, playableTrack}, brokenTrack, ListId{10}));

    REQUIRE(fixture.executor.drainUntil([&] { return currentQueueTrack(queueSession) == playableTrack; }));

    CHECK(queueSession.state().optCurrentIndex);
    CHECK(fixture.playbackService.state().nowPlaying.trackId == playableTrack);
    CHECK(fixture.playbackService.state().nowPlaying.title == "Playable Track");
    REQUIRE(failures.size() == 1);
    CHECK(failures.front().disposition == PlaybackFailureDisposition::Recovered);
    CHECK(hasNotification(fixture.notificationService, NotificationSeverity::Warning, "Skipped 1 unplayable track"));
    CHECK(notificationCount(fixture.notificationService, NotificationSeverity::Error, "Could not play") == 0);
  }

  TEST_CASE("PlaybackQueueService - repeated recoverable playback failures stop the queue",
            "[runtime][unit][playback-queue][error]")
  {
    auto fixture = PlaybackFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const broken1 = fixture.libraryFixture.addTrack({.title = "Broken 1", .uri = "broken1.txt"});
    auto const broken2 = fixture.libraryFixture.addTrack({.title = "Broken 2", .uri = "broken2.txt"});
    auto const broken3 = fixture.libraryFixture.addTrack({.title = "Broken 3", .uri = "broken3.txt"});

    auto queueSession = PlaybackQueueService{fixture.executor, fixture.playbackService, fixture.notificationService};
    auto failures = std::vector<PlaybackFailure>{};
    auto failureSub =
      fixture.playbackService.onPlaybackFailure([&](PlaybackFailure const& failure) { failures.push_back(failure); });
    REQUIRE(queueSession.playQueue({broken1, broken2, broken3}, broken1, ListId{10}));

    REQUIRE(fixture.executor.drainUntil([&] { return !queueSession.state().optCurrentIndex; }));

    CHECK_FALSE(currentQueueTrack(queueSession));
    CHECK(fixture.playbackService.state().nowPlaying.trackId == kInvalidTrackId);
    CHECK(hasNotification(fixture.notificationService, NotificationSeverity::Warning, "Skipped 2 unplayable tracks"));
    CHECK(notificationCount(fixture.notificationService, NotificationSeverity::Error, "Could not play") == 0);
    REQUIRE(failures.size() == 3);
    CHECK(failures[0].disposition == PlaybackFailureDisposition::Recovered);
    CHECK(failures[1].disposition == PlaybackFailureDisposition::Recovered);
    CHECK(failures[2].disposition == PlaybackFailureDisposition::Stopped);
    CHECK(hasNotification(
      fixture.notificationService, NotificationSeverity::Error, "Playback stopped after 3 unplayable tracks"));
  }

  TEST_CASE("PlaybackQueueService - explicit skip resets recoverable failure streak",
            "[runtime][unit][playback-queue][error]")
  {
    auto fixture = PlaybackFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const flacPath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const broken1 = fixture.libraryFixture.addTrack({.title = "Broken 1", .uri = "broken1.txt"});
    auto const playable1 = fixture.libraryFixture.addTrack({.title = "Playable 1", .uri = flacPath});
    auto const broken2 = fixture.libraryFixture.addTrack({.title = "Broken 2", .uri = "broken2.txt"});
    auto const broken3 = fixture.libraryFixture.addTrack({.title = "Broken 3", .uri = "broken3.txt"});
    auto const playable2 = fixture.libraryFixture.addTrack({.title = "Playable 2", .uri = flacPath});

    auto queueSession = PlaybackQueueService{fixture.executor, fixture.playbackService, fixture.notificationService};
    REQUIRE(queueSession.playQueue({broken1, playable1, broken2, broken3, playable2}, broken1, ListId{10}));

    REQUIRE(fixture.executor.drainUntil([&] { return currentQueueTrack(queueSession) == playable1; }));

    queueSession.next();

    REQUIRE(fixture.executor.drainUntil([&] { return currentQueueTrack(queueSession) == playable2; }));

    CHECK(queueSession.state().optCurrentIndex);
    CHECK_FALSE(hasNotification(
      fixture.notificationService, NotificationSeverity::Error, "Playback stopped after 3 unplayable tracks"));
  }

  TEST_CASE("PlaybackQueueService - global synchronous playback failure stops without scanning queue",
            "[runtime][unit][playback-queue][error]")
  {
    auto fixture = PlaybackFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const flacPath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const track1 = fixture.libraryFixture.addTrack({.title = "Track 1", .uri = flacPath});
    auto const track2 = fixture.libraryFixture.addTrack({.title = "Track 2", .uri = flacPath});
    auto const track3 = fixture.libraryFixture.addTrack({.title = "Track 3", .uri = flacPath});

    auto queueSession = PlaybackQueueService{fixture.executor, fixture.playbackService, fixture.notificationService};
    REQUIRE(queueSession.playQueue({track1, track2, track3}, track1, ListId{10}));

    fixture.onDevicesChangedCb({});
    fixture.executor.drain();
    fixture.playbackService.setOutputDevice(
      audio::BackendId{"mock_backend"}, audio::DeviceId{"missing_device"}, audio::ProfileId{audio::kProfileShared});
    REQUIRE_FALSE(fixture.playbackService.state().ready);

    queueSession.next();

    CHECK_FALSE(queueSession.state().optCurrentIndex);
    CHECK(notificationCount(fixture.notificationService, NotificationSeverity::Error, "Could not start playback") == 1);
  }

  TEST_CASE("PlaybackQueueService - device playback failure stops the queue", "[runtime][unit][playback-queue][error]")
  {
    auto fixture = PlaybackFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const flacPath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const currentTrack = fixture.libraryFixture.addTrack({.title = "Current Track", .uri = flacPath});
    auto const nextTrack = fixture.libraryFixture.addTrack({.title = "Next Track", .uri = flacPath});

    auto queueSession = PlaybackQueueService{fixture.executor, fixture.playbackService, fixture.notificationService};
    REQUIRE(queueSession.playQueue({currentTrack, nextTrack}, currentTrack, ListId{10}));
    REQUIRE(fixture.renderTarget != nullptr);

    fixture.renderTarget->handleBackendError("device lost");

    REQUIRE(fixture.executor.drainUntil([&] { return !queueSession.state().optCurrentIndex; }));

    CHECK_FALSE(currentQueueTrack(queueSession));
    CHECK(fixture.playbackService.state().nowPlaying.trackId == kInvalidTrackId);
    CHECK(hasNotification(fixture.notificationService, NotificationSeverity::Error, "Playback device failed"));
  }
} // namespace ao::rt::test
