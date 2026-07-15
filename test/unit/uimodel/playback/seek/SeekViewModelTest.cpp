// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/playback/seek/SeekViewModel.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::test
{
  using namespace ao::rt::test;
  using namespace ao::rt;

  TEST_CASE("SeekViewModel - reactive updates", "[uimodel][unit][playback]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = InlineExecutor{};
    auto notificationService = NotificationService{};
    auto playback = makePlaybackService(executor, libraryFixture.library(), notificationService);
    addReadyAudioProvider(playback);

    auto log = ao::test::RenderLog<SeekViewState>{};
    auto viewModel = SeekViewModel{playback, [&log](auto const& state) { log.render(state); }};

    SECTION("Initial state is insensitive when idle")
    {
      REQUIRE(!log.empty());
      CHECK(log.last().enabled == false);
      CHECK(log.last().duration == std::chrono::milliseconds{0});
      CHECK(log.last().elapsed == std::chrono::milliseconds{0});
    }

    SECTION("refresh with override")
    {
      log.clear();
      viewModel.refresh(true, std::chrono::seconds{5});
      REQUIRE(!log.empty());
      CHECK(log.last().elapsed == std::chrono::seconds{5});
      CHECK(log.last().immediateUpdate == true);
    }

    SECTION("seekPreview/Final")
    {
      auto const trackId = libraryFixture.addTrack({.title = "Seek Test", .artist = "Artist", .album = "Album"});
      auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
      auto const desc = PlaybackService::PlaybackRequest{
        .item = NowPlayingInfo{.trackId = trackId, .title = "Seek Test", .artist = "Artist"},
        .input = audio::PlaybackInput{.filePath = fixturePath, .duration = std::chrono::seconds{5}},
      };
      REQUIRE(playback.play(desc, kInvalidListId));
      auto const expectedDuration = playback.state().duration;
      REQUIRE(expectedDuration > std::chrono::milliseconds{0});

      log.clear();
      viewModel.seekPreview(std::chrono::milliseconds{250});

      REQUIRE(!log.empty());
      CHECK(log.last().duration == expectedDuration);
      CHECK(log.last().elapsed == std::chrono::milliseconds{250});
      CHECK(log.last().isPlaying == true);
      CHECK(log.last().enabled == true);
      CHECK(log.last().immediateUpdate == false);
      CHECK(playback.state().elapsed == std::chrono::milliseconds{0});

      viewModel.seekFinal(std::chrono::milliseconds{500});

      CHECK(log.last().duration == expectedDuration);
      CHECK(log.last().elapsed == std::chrono::milliseconds{500});
      CHECK(log.last().isPlaying == true);
      CHECK(log.last().enabled == true);
      CHECK(log.last().immediateUpdate == true);
      CHECK(playback.state().duration == expectedDuration);
    }
  }
} // namespace ao::uimodel::test
