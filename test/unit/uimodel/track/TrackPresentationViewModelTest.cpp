// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/TestUtils.h"
#include <ao/Type.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/LibraryMutationService.h>
#include <ao/rt/ListSourceStore.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/async/Runtime.h>
#include <ao/uimodel/track/TrackPresentationViewModel.h>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <vector>

namespace ao::uimodel::track::test
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
  }

  TEST_CASE("TrackPresentationViewModel - preset and layout management", "[uimodel][track][presentation]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = NullExecutor{};
    auto runtime = async::Runtime{executor};
    auto mutationService = LibraryMutationService{runtime, testLib.library()};
    auto listSourceStore = ListSourceStore{testLib.library(), mutationService};
    auto viewService = ViewService{executor, testLib.library(), listSourceStore};
    auto playbackService = PlaybackService{executor, viewService, testLib.library()};
    auto workspace = rt::WorkspaceService{viewService, playbackService, mutationService, testLib.library()};

    auto store = TrackPresentationViewModel{workspace};

    SECTION("initial presets")
    {
      CHECK_FALSE(store.builtinPresets().empty());
    }

    SECTION("active presentation management")
    {
      store.setActivePresentationId("default");
      CHECK(store.activePresentationId() == "default");
    }

    SECTION("list layout persistence")
    {
      auto const listId = rt::kAllTracksListId;
      auto const layout = std::vector{ColumnState{.field = rt::TrackField::Title, .width = 200}};

      store.updateLayout(listId, layout);

      auto const& retrieved = store.layoutForList(listId);
      REQUIRE(retrieved.size() == 1);
      CHECK(retrieved[0].field == rt::TrackField::Title);
      CHECK(retrieved[0].width == 200);
    }

    SECTION("list presentation preferences")
    {
      auto const listId = rt::kAllTracksListId;

      CHECK_FALSE(store.presentationIdForList(listId));

      store.setPresentationIdForList(listId, "albums");

      auto const optId = store.presentationIdForList(listId);
      REQUIRE(optId);
      CHECK(*optId == "albums");
      CHECK(store.presentationForList(listId).id == "albums");

      store.clearPresentationForList(listId);
      CHECK_FALSE(store.presentationIdForList(listId));
    }

    SECTION("invalid list id presentation preference is ignored")
    {
      store.setPresentationIdForList(kInvalidListId, "albums");

      CHECK_FALSE(store.presentationIdForList(kInvalidListId));
    }

    SECTION("signal propagation on change")
    {
      auto changed = false;
      auto sub = store.signalChanged().connect([&](auto, auto) { changed = true; });

      store.setActivePresentationId("modern");
      CHECK(changed == true);
    }
  }
} // namespace ao::uimodel::track::test
