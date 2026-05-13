// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

#include <ao/Type.h>
#include <ao/library/ListLayout.h>
#include <cstddef>

namespace ao::library::test
{
  TEST_CASE("ListHeader - Size and Alignment")
  {
    CHECK(sizeof(ListHeader) == 20);
    CHECK(alignof(ListHeader) == 4);
  }

  TEST_CASE("ListHeader - Field Offsets")
  {
    CHECK(offsetof(ListHeader, trackIdsCount) == 0);
    CHECK(offsetof(ListHeader, nameOffset) == 4);
    CHECK(offsetof(ListHeader, nameLen) == 6);
    CHECK(offsetof(ListHeader, descOffset) == 8);
    CHECK(offsetof(ListHeader, descLen) == 10);
    CHECK(offsetof(ListHeader, filterOffset) == 12);
    CHECK(offsetof(ListHeader, filterLen) == 14);
    CHECK(offsetof(ListHeader, parentId) == 16);
  }
} // namespace ao::library::test
