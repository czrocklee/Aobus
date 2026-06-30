// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include <ao/CoreIds.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/source/ListSourceStore.h>
#include <ao/uimodel/playback/seek/SeekViewModel.h>

#include <catch2/catch_test_macros.hpp>

#include <functional>

namespace ao::uimodel::test
{
  using namespace ao::rt::test;
  using namespace ao::rt;

  TEST_CASE("SeekViewModel - reactive updates", "[uimodel][unit][playback]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto changes = LibraryChanges{};
    auto listSourceStore = ListSourceStore{testLib.library(), changes};
    auto viewService = ViewService{executor, testLib.library(), listSourceStore};
    auto playback = PlaybackService{executor, viewService, testLib.library()};
    addReadyAudioProvider(playback);

    auto log = RenderLog<SeekViewState>{};
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
      auto const trackId = testLib.addTrack({.title = "Seek Test", .artist = "Artist", .album = "Album"});
      auto const desc = PlaybackService::PlaybackRequest{
        .trackId = trackId,
        .input = audio::PlaybackInput{.duration = std::chrono::seconds{5}},
        .title = "Seek Test",
        .artist = "Artist",
      };
      REQUIRE(playback.play(desc, kInvalidListId));

      log.clear();
      viewModel.seekPreview(std::chrono::seconds{1});

      REQUIRE(!log.empty());
      CHECK(log.last().duration == std::chrono::seconds{5});
      CHECK(log.last().elapsed == std::chrono::seconds{1});
      CHECK(log.last().isPlaying == false);
      CHECK(log.last().enabled == true);
      CHECK(log.last().immediateUpdate == false);
      CHECK(playback.state().elapsed == std::chrono::milliseconds{0});

      viewModel.seekFinal(std::chrono::seconds{2});

      CHECK(log.last().duration == std::chrono::seconds{5});
      CHECK(log.last().elapsed == std::chrono::seconds{2});
      CHECK(log.last().isPlaying == false);
      CHECK(log.last().enabled == true);
      CHECK(log.last().immediateUpdate == true);
      CHECK(playback.state().duration == std::chrono::seconds{5});
    }
  }
} // namespace ao::uimodel::test
