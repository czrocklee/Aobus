// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/TestUtils.h"
#include <ao/rt/PlaybackService.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/source/ListSourceStore.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#include <catch2/catch_test_macros.hpp>

#include <functional>

namespace ao::uimodel::test
{
  using namespace ao::rt::test;
  using namespace ao::rt;

  TEST_CASE("AobusSoulViewModel - view state generation", "[uimodel][unit][playback]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto changes = LibraryChanges{};
    auto listSourceStore = ListSourceStore{testLib.library(), changes};
    auto viewService = ViewService{executor, testLib.library(), listSourceStore};
    auto playback = PlaybackService{executor, viewService, testLib.library()};

    auto log = ao::test::RenderLog<AobusSoulViewState>{};
    auto const viewModel = AobusSoulViewModel{playback, [&log](auto const& view) { log.render(view); }};

    SECTION("Initial render when idle")
    {
      REQUIRE(!log.empty());
      CHECK(log.last().isBreathing == false);
      CHECK(log.last().auraColor == AuraColor::Idle);
    }
  }
} // namespace ao::uimodel::test
