// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/TestUtils.h"
#include <ao/Type.h>
#include <ao/audio/Types.h>
#include <ao/rt/LibraryMutationService.h>
#include <ao/rt/ListSourceStore.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/async/Runtime.h>
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
      CHECK(log.last().positionMs == 0);
      CHECK(log.last().durationMs == 0);
    }

    SECTION("onSeekUpdate triggers refresh with Preview and Final modes")
    {
      auto const trackId = testLib.addTrack({.title = "Seek Test", .artist = "Artist", .album = "Album"});
      auto desc = audio::TrackPlaybackDescriptor{.trackId = trackId, .filePath = "test.flac", .durationMs = 30000};
      playback.play(desc, kInvalidListId);

      log.clear();
      playback.seek(5000, PlaybackService::SeekMode::Final);
      REQUIRE(!log.empty());
      CHECK(log.last().positionMs > 0);
      CHECK(log.last().immediateUpdate == true);

      log.clear();
      playback.seek(10000, PlaybackService::SeekMode::Preview);
      REQUIRE(!log.empty());
    }
  }
} // namespace ao::uimodel::playback::test
