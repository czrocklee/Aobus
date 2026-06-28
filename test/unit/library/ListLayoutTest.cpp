// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/library/ListLayout.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

namespace ao::library::test
{
  TEST_CASE("ListHeader - has stable size and alignment", "[library][unit][list]")
  {
    CHECK(sizeof(ListHeader) == 20);
    CHECK(alignof(ListHeader) == 4);
  }

  TEST_CASE("ListHeader - stores fields at stable offsets", "[library][unit][list]")
  {
    CHECK(offsetof(ListHeader, trackIdsCount) == 0);
    CHECK(offsetof(ListHeader, nameOffset) == 4);
    CHECK(offsetof(ListHeader, nameLength) == 6);
    CHECK(offsetof(ListHeader, descOffset) == 8);
    CHECK(offsetof(ListHeader, descLength) == 10);
    CHECK(offsetof(ListHeader, filterOffset) == 12);
    CHECK(offsetof(ListHeader, filterLength) == 14);
    CHECK(offsetof(ListHeader, parentId) == 16);
  }
} // namespace ao::library::test
