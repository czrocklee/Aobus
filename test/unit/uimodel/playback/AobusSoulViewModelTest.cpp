// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/TestUtils.h"
#include <ao/async/Runtime.h>
#include <ao/rt/LibraryMutationService.h>
#include <ao/rt/ListSourceStore.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/ViewService.h>
#include <ao/uimodel/playback/AobusSoulViewModel.h>

#include <catch2/catch_test_macros.hpp>

#include <functional>

namespace ao::uimodel::playback::test
{
  using namespace ao::rt::test;
  using namespace ao::rt;

  TEST_CASE("AobusSoulViewModel - view state generation", "[unit][uimodel][playback]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto runtime = async::Runtime{executor};
    auto mutationService = LibraryMutationService{runtime, testLib.library()};
    auto listSourceStore = ListSourceStore{testLib.library(), mutationService};
    auto viewService = ViewService{executor, testLib.library(), listSourceStore};
    auto playback = PlaybackService{executor, viewService, testLib.library()};

    auto log = RenderLog<AobusSoulViewState>{};
    auto const viewModel = AobusSoulViewModel{playback, [&log](auto const& view) { log.render(view); }};

    SECTION("Initial render when idle")
    {
      REQUIRE(!log.empty());
      CHECK(log.last().isBreathing == false);
      CHECK(log.last().auraColor == AuraColor::Idle);
    }
  }
} // namespace ao::uimodel::playback::test
