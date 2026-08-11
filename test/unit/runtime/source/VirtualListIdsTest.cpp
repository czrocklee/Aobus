// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/VirtualListIds.h>

#include <ao/CoreIds.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::rt::test
{
  TEST_CASE("VirtualListIds - identifies IDs that do not reference a user-created list", "[runtime][unit][source]")
  {
    CHECK(isVirtualListId(kInvalidListId));
    CHECK(isVirtualListId(kAllTracksListId));
    CHECK_FALSE(isVirtualListId(ListId{1}));
    CHECK_FALSE(isVirtualListId(ListId{42}));
  }

  TEST_CASE("VirtualListIds - resolves a list parent to the source it derives from", "[runtime][unit][source]")
  {
    CHECK(resolveParentSourceId(kInvalidListId) == kAllTracksListId);
    CHECK(resolveParentSourceId(kAllTracksListId) == kAllTracksListId);
    CHECK(resolveParentSourceId(ListId{42}) == ListId{42});
  }
} // namespace ao::rt::test
