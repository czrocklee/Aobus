// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch.hpp>

#include <cstddef>
#include <rs/core/ListLayout.h>
#include <rs/core/Type.h>

namespace
{
  using rs::core::ListHeader;

  TEST_CASE("ListHeader - Size and Alignment")
  {
    CHECK(sizeof(ListHeader) == 16);
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
  }

} // anonymous namespace
