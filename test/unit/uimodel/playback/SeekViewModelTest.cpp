// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/TestUtils.h"
#include <ao/rt/LibraryMutationService.h>
#include <ao/rt/ListSourceStore.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/async/Runtime.h>
#include <ao/uimodel/playback/SeekViewModel.h>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <vector>

namespace ao::uimodel::playback::test
{
  using namespace ao::rt::test;
  using namespace ao::rt;
  namespace
  {
    struct NullExecutor final : public IControlExecutor
    {
      bool isCurrent() const noexcept override { return true; }
      void dispatch(std::move_only_function<void()> task) override { task(); }
      void defer(std::move_only_function<void()> task) override { task(); }
    };

    template<typename T>
    struct RenderLog final
    {
      std::vector<T> states;
      void render(T const& state) { states.push_back(state); }
      T const& last() const { return states.back(); }
      bool empty() const { return states.empty(); }
      void clear() { states.clear(); }
    };
  }

  TEST_CASE("SeekViewModel - reactive updates", "[unit][uimodel][playback]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = NullExecutor{};
    auto runtime = async::Runtime{executor};
    auto mutationService = LibraryMutationService{runtime, testLib.library()};
    auto listSourceStore = ListSourceStore{testLib.library(), mutationService};
    auto viewService = ViewService{executor, testLib.library(), listSourceStore};
    auto playback = PlaybackService{executor, viewService, testLib.library()};

    auto log = RenderLog<SeekViewState>{};
    auto viewModel = SeekViewModel{playback, [&log](auto const& state) { log.render(state); }};

    SECTION("Initial state is insensitive when idle")
    {
      REQUIRE(!log.empty());
      CHECK(log.last().enabled == false);
      CHECK(log.last().durationMs == 0);
      CHECK(log.last().positionMs == 0);
    }

    SECTION("refresh with override")
    {
      log.clear();
      viewModel.refresh(true, 5000);
      REQUIRE(!log.empty());
      CHECK(log.last().positionMs == 5000);
      CHECK(log.last().immediateUpdate == true);
    }

    SECTION("seekPreview/Final")
    {
      viewModel.seekPreview(1000);
      viewModel.seekFinal(2000);
    }
  }
} // namespace ao::uimodel::playback::test
