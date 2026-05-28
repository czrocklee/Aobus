// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/TestUtils.h"
#include <ao/rt/LibraryMutationService.h>
#include <ao/rt/ListSourceStore.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/async/Runtime.h>
#include <ao/uimodel/playback/VolumeViewModel.h>

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

  TEST_CASE("VolumeViewModel - view state generation", "[unit][uimodel][playback]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = NullExecutor{};
    auto runtime = async::Runtime{executor};
    auto mutationService = LibraryMutationService{runtime, testLib.library()};
    auto listSourceStore = ListSourceStore{testLib.library(), mutationService};
    auto viewService = ViewService{executor, testLib.library(), listSourceStore};
    auto playback = PlaybackService{executor, viewService, testLib.library()};

    auto log = RenderLog<VolumeViewState>{};
    auto viewModel = VolumeViewModel{playback, [&log](auto const& view) { log.render(view); }};

    SECTION("Initial render")
    {
      REQUIRE(!log.empty());
    }

    SECTION("handleVolumeChanged delegates to playback")
    {
      viewModel.handleVolumeChanged(0.35F);
      CHECK(playback.state().volume == 0.35F);
    }
  }

  TEST_CASE("VolumeViewModel - math helpers", "[unit][uimodel][playback]")
  {
    double const kWidth = 100.0;

    SECTION("resolveVolumeOffset")
    {
      CHECK(VolumeViewModel::resolveVolumeOffset(0.0, 50.0, 0.5F) == 0.5F);
      CHECK(VolumeViewModel::resolveVolumeOffset(kWidth, 0.0) == 0.0F);
      CHECK(VolumeViewModel::resolveVolumeOffset(kWidth, 50.0) == 0.5F);
      CHECK(VolumeViewModel::resolveVolumeOffset(kWidth, 100.0) == 1.0F);

      CHECK(VolumeViewModel::resolveVolumeOffset(kWidth, 25.0, 0.5F) == 0.75F);
      CHECK(VolumeViewModel::resolveVolumeOffset(kWidth, -25.0, 0.5F) == 0.25F);

      CHECK(VolumeViewModel::resolveVolumeOffset(kWidth, 150.0) == 1.0F);
      CHECK(VolumeViewModel::resolveVolumeOffset(kWidth, -50.0) == 0.0F);
    }

    SECTION("resolveVolumeScroll")
    {
      CHECK(VolumeViewModel::resolveVolumeScroll(0.5F, 1.0) == 0.45F);
      CHECK(VolumeViewModel::resolveVolumeScroll(0.5F, -1.0) == 0.55F);
      CHECK(VolumeViewModel::resolveVolumeScroll(0.0F, 1.0) == 0.0F);
      CHECK(VolumeViewModel::resolveVolumeScroll(1.0F, -1.0) == 1.0F);
    }
  }
} // namespace ao::uimodel::playback::test
