// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/TestUtils.h"
#include <ao/Type.h>
#include <ao/async/Runtime.h>
#include <ao/audio/Types.h>
#include <ao/rt/LibraryMutationService.h>
#include <ao/rt/ListSourceStore.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/ViewService.h>
#include <ao/uimodel/playback/PlaybackQueueModel.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <vector>

namespace ao::uimodel::playback::test
{
  using namespace ao::rt::test;
  using namespace ao::rt;

  TEST_CASE("PlaybackQueueModel - basic controls", "[unit][runtime][uimodel][playback]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto runtime = async::Runtime{executor};
    auto mutationService = LibraryMutationService{runtime, testLib.library()};
    auto listSourceStore = ListSourceStore{testLib.library(), mutationService};
    auto viewService = ViewService{executor, testLib.library(), listSourceStore};

    auto playbackService = PlaybackService{executor, viewService, testLib.library()};

    auto providerCalledCount = 0;
    auto descriptorProvider = [&](TrackId id) -> std::optional<audio::TrackPlaybackDescriptor>
    {
      providerCalledCount++;

      if (id.raw() == 999)
      {
        return std::nullopt; // simulate failure
      }

      return audio::TrackPlaybackDescriptor{
        .trackId = id, .filePath = "/test/path", .title = "Test", .artist = "Artist", .durationMs = 120000};
    };

    auto queueModel = PlaybackQueueModel{playbackService, descriptorProvider};

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
      auto const tracks = std::vector{TrackId{1}, TrackId{2}};
      auto const result = queueModel.playQueue(tracks, TrackId{3}, ListId{10});
      CHECK(result == false);
      CHECK(queueModel.isActive() == false);
    }

    SECTION("playQueue sets up the queue correctly")
    {
      auto const tracks = std::vector{TrackId{1}, TrackId{2}, TrackId{3}};
      auto const result = queueModel.playQueue(tracks, TrackId{2}, ListId{10});

      CHECK(result == true);
      CHECK(queueModel.isActive() == true);
      CHECK(queueModel.nowPlayingTrackId() == TrackId{2});
      CHECK(queueModel.sourceListId() == ListId{10});
      CHECK(playbackService.state().trackId == TrackId{2});
    }

    SECTION("playQueue fails on empty list")
    {
      auto const tracks = std::vector<TrackId>{};
      auto const result = queueModel.playQueue(tracks, TrackId{2}, ListId{10});
      CHECK(result == false);
    }

    SECTION("playQueue fails on missing descriptor")
    {
      auto const tracks = std::vector{TrackId{999}};
      auto const result = queueModel.playQueue(tracks, TrackId{999}, ListId{10});
      CHECK(result == false);
    }

    SECTION("advanceToNext when active and idle")
    {
      auto const tracks = std::vector{TrackId{1}, TrackId{2}, TrackId{3}};
      queueModel.playQueue(tracks, TrackId{1}, ListId{10});
      CHECK(queueModel.nowPlayingTrackId() == TrackId{1});

      queueModel.next();
      CHECK(queueModel.nowPlayingTrackId() == TrackId{2});

      queueModel.next();
      CHECK(queueModel.nowPlayingTrackId() == TrackId{3});

      // End of list, no repeat
      queueModel.next();
      CHECK(queueModel.isActive() == false);
    }

    SECTION("previous when active")
    {
      auto const tracks = std::vector{TrackId{1}, TrackId{2}, TrackId{3}};
      queueModel.playQueue(tracks, TrackId{2}, ListId{10});
      CHECK(queueModel.nowPlayingTrackId() == TrackId{2});

      queueModel.previous();
      CHECK(queueModel.nowPlayingTrackId() == TrackId{1});

      // At start of list, no repeat
      queueModel.previous();
      CHECK(queueModel.nowPlayingTrackId() == TrackId{1}); // doesn't wrap around
    }

    SECTION("Repeat One")
    {
      queueModel.setRepeatMode(rt::RepeatMode::One);
      auto const tracks1 = std::vector{TrackId{1}, TrackId{2}};
      queueModel.playQueue(tracks1, TrackId{1}, ListId{10});

      queueModel.next();
      CHECK(queueModel.nowPlayingTrackId() == TrackId{1});
    }

    SECTION("Repeat All")
    {
      queueModel.setRepeatMode(rt::RepeatMode::All);
      auto const tracks2 = std::vector{TrackId{1}, TrackId{2}};
      queueModel.playQueue(tracks2, TrackId{2}, ListId{10});

      queueModel.next();
      CHECK(queueModel.nowPlayingTrackId() == TrackId{1});

      queueModel.previous();
      CHECK(queueModel.nowPlayingTrackId() == TrackId{2});
    }

    SECTION("Previous at start with Repeat All wraps to end")
    {
      queueModel.setRepeatMode(rt::RepeatMode::All);
      auto const tracks = std::vector{TrackId{1}, TrackId{2}, TrackId{3}};
      queueModel.playQueue(tracks, TrackId{1}, ListId{10});
      CHECK(queueModel.nowPlayingTrackId() == TrackId{1});

      queueModel.previous();
      CHECK(queueModel.nowPlayingTrackId() == TrackId{3});
    }

    SECTION("Previous skips invalid tracks")
    {
      // Setup: [1, 999, 3] where 999 is invalid
      auto const tracks = std::vector{TrackId{1}, TrackId{999}, TrackId{3}};
      queueModel.playQueue(tracks, TrackId{3}, ListId{10});
      CHECK(queueModel.nowPlayingTrackId() == TrackId{3});

      // From 3, go previous. Should skip 999 and find 1.
      queueModel.previous();
      CHECK(queueModel.nowPlayingTrackId() == TrackId{1});
    }

    SECTION("Previous at start with Repeat All skips invalid tracks at end")
    {
      queueModel.setRepeatMode(rt::RepeatMode::All);
      // Setup: [1, 2, 999] where 999 is invalid
      auto const tracks = std::vector{TrackId{1}, TrackId{2}, TrackId{999}};
      queueModel.playQueue(tracks, TrackId{1}, ListId{10});
      CHECK(queueModel.nowPlayingTrackId() == TrackId{1});

      // From 1, go previous. Should wrap to end, skip 999, and find 2.
      queueModel.previous();
      CHECK(queueModel.nowPlayingTrackId() == TrackId{2});
    }

    SECTION("Shuffle mode next picks random track")
    {
      queueModel.setShuffleMode(rt::ShuffleMode::On);
      auto const tracks = std::vector{TrackId{1}, TrackId{2}, TrackId{3}};
      queueModel.playQueue(tracks, TrackId{1}, ListId{10});

      queueModel.next();
      CHECK(queueModel.isActive() == true);
    }
  }
} // namespace ao::uimodel::playback::test
