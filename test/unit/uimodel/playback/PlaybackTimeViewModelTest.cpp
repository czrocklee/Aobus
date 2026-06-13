// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/TestUtils.h"
#include <ao/Type.h>
#include <ao/async/Runtime.h>
#include <ao/audio/Types.h>
#include <ao/rt/LibraryMutationService.h>
#include <ao/rt/ListSourceStore.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/ViewService.h>
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
    auto runtime = async::Runtime{executor};
    auto mutationService = LibraryMutationService{runtime, testLib.library()};
    auto listSourceStore = ListSourceStore{testLib.library(), mutationService};
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
      auto desc = audio::TrackPlaybackDescriptor{
        .trackId = trackId, .filePath = "test.flac", .duration = std::chrono::seconds{30}};
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
