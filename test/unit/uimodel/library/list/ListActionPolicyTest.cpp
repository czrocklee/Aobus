// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/uimodel/library/list/ListActionPolicy.h>

#include <catch2/catch_test_macros.hpp>

using namespace ao;

namespace ao::uimodel::test
{
  TEST_CASE("ListActionPolicy describes available actions from selected list state", "[uimodel][unit][list]")
  {
    SECTION("Invalid list selection")
    {
      auto const state = ListActionPolicy::describeActions(kInvalidListId, false);
      CHECK(state.canCreate == true);
      CHECK(state.canEdit == false);
      CHECK(state.canDelete == false);
    }

    SECTION("All Tracks list selection (system reserved)")
    {
      auto const state = ListActionPolicy::describeActions(rt::kAllTracksListId, false);
      CHECK(state.canCreate == true);
      CHECK(state.canEdit == false);
      CHECK(state.canDelete == false);
    }

    SECTION("Normal list selection without children")
    {
      auto const state = ListActionPolicy::describeActions(ListId{1}, false);
      CHECK(state.canCreate == true);
      CHECK(state.canEdit == true);
      CHECK(state.canDelete == true);
    }

    SECTION("Normal list selection with children")
    {
      auto const state = ListActionPolicy::describeActions(ListId{1}, true);
      CHECK(state.canCreate == true);
      CHECK(state.canEdit == true);
      CHECK(state.canDelete == false); // Cannot delete list with children
    }
  }

  TEST_CASE("ListActionPolicy chooses a parent only for user-created list selections", "[uimodel][unit][list]")
  {
    CHECK(ListActionPolicy::parentForNewSmartList(kInvalidListId) == kInvalidListId);
    CHECK(ListActionPolicy::parentForNewSmartList(rt::kAllTracksListId) == kInvalidListId);
    CHECK(ListActionPolicy::parentForNewSmartList(ListId{42}) == ListId{42});
  }
} // namespace ao::uimodel::test
