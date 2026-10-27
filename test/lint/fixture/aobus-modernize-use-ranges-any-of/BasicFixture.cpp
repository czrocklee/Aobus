// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <vector>

std::vector<int>::const_iterator legend(std::vector<int> const& v);

#define AOBUS_HAS_POSITIVE(c, p) (std::ranges::find_if(c, p) != c.end())

void testAnyAll(std::vector<int> const& v, std::vector<int> const& w)
{
  auto pred = [](std::int32_t x) { return x > 0; };

  // POSITIVE | use std::ranges::any_of instead of std::ranges::find_if != end()
  if (std::ranges::find_if(v, pred) != v.end())
  {
  }

  // POSITIVE | use std::ranges::any_of instead of std::ranges::find_if != end()
  if (std::ranges::find_if(v, [](std::int32_t x) { return x > 0; }) != std::ranges::end(v))
  {
  }

  // POSITIVE | use std::ranges::any_of instead of std::ranges::find_if != end()
  bool const hasPositive = std::ranges::find_if(v, pred) != std::end(v);

  // NEGATIVE
  if (std::ranges::any_of(v, pred))
  {
  }

  // POSITIVE | use std::ranges::none_of instead of std::ranges::find_if == end()
  if (std::ranges::find_if(v, pred) == v.end())
  {
  }

  // POSITIVE | use std::ranges::all_of instead of std::ranges::find_if_not == end()
  if (std::ranges::find_if_not(v, pred) == v.end())
  {
  }

  // POSITIVE | use !std::ranges::all_of instead of std::ranges::find_if_not != end()
  if (std::ranges::find_if_not(v, pred) != v.end())
  {
  }

  // POSITIVE | use std::ranges::any_of instead of std::ranges::find_if != end()
  if (std::ranges::find_if(v, pred) != v.cend())
  {
  }

  // NEGATIVE - legend() is not an end() accessor even though its name contains "end"
  if (std::ranges::find_if(v, pred) != legend(v))
  {
  }

  // NEGATIVE - end() of a different container; rewriting would drop the reference to w
  if (std::ranges::find_if(v, pred) != w.end())
  {
  }

  // NEGATIVE - the comparison is spelled inside a macro expansion
  if (AOBUS_HAS_POSITIVE(v, pred))
  {
  }

  // NEGATIVE
  auto it = std::ranges::find_if(v, pred);

  if (it != v.end())
  {
    // using iterator, should not be converted
  }
}
