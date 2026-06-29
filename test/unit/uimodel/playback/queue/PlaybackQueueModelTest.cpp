// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include <ao/Type.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/source/ListSourceStore.h>
#include <ao/uimodel/playback/queue/PlaybackQueueModel.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <vector>

namespace ao::uimodel::test
{
  using namespace ao::rt::test;
  using namespace ao::rt;

  TEST_CASE("PlaybackQueueModel - basic controls", "[uimodel][unit][playback]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto changes = LibraryChanges{};
    auto listSourceStore = ListSourceStore{testLib.library(), changes};
    auto viewService = ViewService{executor, testLib.library(), listSourceStore};

    auto playbackService = PlaybackService{executor, viewService, testLib.library()};
    addReadyAudioProvider(playbackService);

    auto const track1 = testLib.addTrack({.title = "Track 1"});
    auto const track2 = testLib.addTrack({.title = "Track 2"});
    auto const track3 = testLib.addTrack({.title = "Track 3"});
    auto const missingTrack = TrackId{999};

    auto queueModel = PlaybackQueueModel{playbackService};

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
  }
} // namespace ao::uimodel::test
