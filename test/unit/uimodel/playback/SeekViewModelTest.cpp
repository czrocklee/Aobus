// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/TestUtils.h"
#include <ao/rt/PlaybackService.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/source/ListSourceStore.h>
#include <ao/uimodel/playback/SeekViewModel.h>

#include <catch2/catch_test_macros.hpp>

#include <functional>

namespace ao::uimodel::playback::test
{
  using namespace ao::rt::test;
  using namespace ao::rt;

  TEST_CASE("SeekViewModel - reactive updates", "[unit][uimodel][playback]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto changes = LibraryChanges{};
    auto listSourceStore = ListSourceStore{testLib.library(), changes};
    auto viewService = ViewService{executor, testLib.library(), listSourceStore};
    auto playback = PlaybackService{executor, viewService, testLib.library()};

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
      viewModel.seekPreview(std::chrono::seconds{1});
      viewModel.seekFinal(std::chrono::seconds{2});
    }
  }
} // namespace ao::uimodel::playback::test
