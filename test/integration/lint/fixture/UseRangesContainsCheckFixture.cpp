// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <vector>

void testFind(std::vector<int> const& v)
{
  // POSITIVE
  if (std::ranges::find(v, 5) != v.end())
  {
  }

  // POSITIVE
  if (std::ranges::find(v, 6) == std::ranges::end(v))
  {
  }

  // POSITIVE
  bool const found = std::ranges::find(v, 7) != std::end(v);

  // NEGATIVE
  if (std::ranges::contains(v, 8))
  {
  }
}

void testCount(std::vector<int> const& v)
{
  // POSITIVE
  if (std::ranges::count(v, 5) > 0)
  {
  }

  // POSITIVE
  if (std::ranges::count(v, 6) != 0)
  {
  }

  // POSITIVE
  if (std::ranges::count(v, 7) == 0)
  {
  }

  // NEGATIVE
  std::int32_t const c = std::ranges::count(v, 8);
}
