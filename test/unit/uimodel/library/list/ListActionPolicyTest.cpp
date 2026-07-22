// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/uimodel/library/list/ListActionPolicy.h>

#include <catch2/catch_test_macros.hpp>

using namespace ao;

namespace ao::uimodel::test
{
  TEST_CASE("ListActionPolicy - describes available actions from selected list state", "[uimodel][unit][list]")
  {
    SECTION("Invalid list selection")
    {
      auto const state = describeListActions(kInvalidListId, false);
      CHECK(state.canCreate == true);
      CHECK(state.canEdit == false);
      CHECK(state.canDelete == false);
    }

    SECTION("All Tracks list selection (system reserved)")
    {
      auto const state = describeListActions(rt::kAllTracksListId, false);
      CHECK(state.canCreate == true);
      CHECK(state.canEdit == false);
      CHECK(state.canDelete == false);
    }

    SECTION("Normal list selection without children")
    {
      auto const state = describeListActions(ListId{1}, false);
      CHECK(state.canCreate == true);
      CHECK(state.canEdit == true);
      CHECK(state.canDelete == true);
    }

    SECTION("Normal list selection with children")
    {
      auto const state = describeListActions(ListId{1}, true);
      CHECK(state.canCreate == true);
      CHECK(state.canEdit == true);
      CHECK(state.canDelete == false); // Cannot delete list with children
    }
  }

  TEST_CASE("ListActionPolicy - chooses a parent only for user-created list selections", "[uimodel][unit][list]")
  {
    CHECK(parentForNewSmartList(kInvalidListId) == kInvalidListId);
    CHECK(parentForNewSmartList(rt::kAllTracksListId) == kInvalidListId);
    CHECK(parentForNewSmartList(ListId{42}) == ListId{42});
  }
} // namespace ao::uimodel::test
