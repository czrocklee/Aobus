// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include <ao/Type.h>
#include <ao/audio/Types.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/source/ListSourceStore.h>
#include <ao/uimodel/playback/PlaybackTimeViewModel.h>

#include <catch2/catch_test_macros.hpp>

#include <functional>

namespace ao::uimodel::playback::test
{
  using namespace ao::rt::test;
  using namespace ao::rt;

  TEST_CASE("PlaybackTimeViewModel - view state generation", "[unit][uimodel][playback]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto changes = LibraryChanges{};
    auto listSourceStore = ListSourceStore{testLib.library(), changes};
    auto viewService = ViewService{executor, testLib.library(), listSourceStore};
    auto playback = PlaybackService{executor, viewService, testLib.library()};

    auto log = RenderLog<PlaybackTimeViewState>{};
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
      auto desc = PlaybackService::PlaybackRequest{
        .trackId = trackId,
        .input = audio::PlaybackInput{.filePath = "test.flac", .duration = std::chrono::seconds{30}},
        .title = "Seek Test",
        .artist = "Artist",
      };
      playback.play(desc, kInvalidListId);

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
} // namespace ao::uimodel::playback::test
