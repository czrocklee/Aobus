// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureUtils.h"
#include <ao/CoreIds.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/source/ListSourceStore.h>
#include <ao/uimodel/playback/seek/PlaybackTimeViewModel.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace ao::uimodel::test
{
  using namespace ao::rt::test;
  using namespace ao::rt;

  TEST_CASE("PlaybackTimeViewModel - view state generation", "[uimodel][unit][playback]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto changes = LibraryChanges{};
    auto listSourceStore = ListSourceStore{testLib.library(), changes};
    auto viewService = ViewService{executor, testLib.library(), listSourceStore};
    auto notificationService = NotificationService{};
    auto playback = PlaybackService{executor, viewService, testLib.library(), notificationService};
    addReadyAudioProvider(playback);

    auto log = ao::test::RenderLog<PlaybackTimeViewState>{};
    auto const viewModel = PlaybackTimeViewModel{playback, [&log](auto const& view) { log.render(view); }};

    SECTION("Initial render")
    {
      REQUIRE(!log.empty());
      CHECK(log.last().elapsed == std::chrono::milliseconds{0});
      CHECK(log.last().duration == std::chrono::milliseconds{0});
    }

    SECTION("onSeekUpdate triggers refresh with Preview and Final modes")
    {
      auto const trackId = testLib.addTrack({.title = "Seek Test", .artist = "Artist", .album = "Album"});
      auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
      auto desc = PlaybackService::PlaybackRequest{
        .item = NowPlayingInfo{.trackId = trackId, .title = "Seek Test", .artist = "Artist"},
        .input = audio::PlaybackInput{.filePath = fixturePath, .duration = std::chrono::seconds{30}},
      };
      REQUIRE(playback.play(desc, kInvalidListId));

      log.clear();
      playback.seek(std::chrono::seconds{5}, PlaybackService::SeekMode::Final);
      REQUIRE(!log.empty());
      CHECK(log.last().elapsed > std::chrono::milliseconds{0});
      CHECK(log.last().immediateUpdate == true);

      log.clear();
      playback.seek(std::chrono::seconds{10}, PlaybackService::SeekMode::Preview);
      REQUIRE(!log.empty());
    }
  }

  TEST_CASE("PlaybackTimeViewModel formats display text for each label mode", "[uimodel][unit][playback]")
  {
    SECTION("template text reserves the widest idle label")
    {
      CHECK(PlaybackTimeViewModel::describeTimeTemplate(PlaybackTimeMode::Elapsed) == "00:00");
      CHECK(PlaybackTimeViewModel::describeTimeTemplate(PlaybackTimeMode::Duration) == "00:00");
      CHECK(PlaybackTimeViewModel::describeTimeTemplate(PlaybackTimeMode::Default) == "00:00 / 00:00");
    }

    SECTION("playback time text matches selected label mode")
    {
      auto const elapsed = std::chrono::seconds{65};
      auto const duration = std::chrono::hours{1} + std::chrono::minutes{1} + std::chrono::seconds{1};

      CHECK(PlaybackTimeViewModel::formatPlaybackTime(PlaybackTimeMode::Elapsed, elapsed, duration) == "1:05");
      CHECK(PlaybackTimeViewModel::formatPlaybackTime(PlaybackTimeMode::Duration, elapsed, duration) == "61:01");
      CHECK(PlaybackTimeViewModel::formatPlaybackTime(PlaybackTimeMode::Default, elapsed, duration) == "1:05 / 61:01");
    }
  }
} // namespace ao::uimodel::test
