// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <vector>

// Not an end() accessor even though its name contains "end".
std::vector<int>::const_iterator legend(std::vector<int> const& v);

namespace mylib
{
  // Same unqualified name as std::ranges::find, different semantics.
  std::vector<int>::const_iterator find(std::vector<int> const& v, int val);
} // namespace mylib

#define AOBUS_COUNT(c, x) std::ranges::count(c, x)

void testFind(std::vector<int> const& v, std::vector<int> const& w)
{
  // POSITIVE: FIX-TO: if (std::ranges::contains(v, 5))
  if (std::ranges::find(v, 5) != v.end())
  {
  }

  // POSITIVE: FIX-TO: if (!std::ranges::contains(v, 6))
  if (std::ranges::find(v, 6) == std::ranges::end(v))
  {
  }

  // POSITIVE: FIX-TO: bool const found = std::ranges::contains(v, 7);
  bool const found = std::ranges::find(v, 7) != std::end(v);

  // POSITIVE: FIX-TO: if (std::ranges::contains(v, 9))
  if (std::ranges::find(v, 9) != v.cend())
  {
  }

  // NEGATIVE
  if (std::ranges::contains(v, 8))
  {
  }

  // NEGATIVE - legend() is not an end() accessor even though its name contains "end"
  if (std::ranges::find(v, 5) != legend(v))
  {
  }

  // NEGATIVE - end() of a different container; rewriting would drop the reference to w
  if (std::ranges::find(v, 5) != w.end())
  {
  }

  // NEGATIVE - iterator-pair overload; the first argument is not a range
  if (std::ranges::find(v.begin(), v.end(), 5) != v.end())
  {
  }

  // NEGATIVE - same name, but not std::ranges::find
  if (mylib::find(v, 5) != v.end())
  {
  }
}

void testCount(std::vector<int> const& v)
{
  // POSITIVE: FIX-TO: if (std::ranges::contains(v, 5))
  if (std::ranges::count(v, 5) > 0)
  {
  }

  // POSITIVE: FIX-TO: if (std::ranges::contains(v, 6))
  if (std::ranges::count(v, 6) != 0)
  {
  }

  // POSITIVE: FIX-TO: if (!std::ranges::contains(v, 7))
  if (std::ranges::count(v, 7) == 0)
  {
  }

  // NEGATIVE
  std::int32_t const c = std::ranges::count(v, 8);

  // NEGATIVE - the comparison is spelled inside a macro expansion
  if (AOBUS_COUNT(v, 5) > 0)
  {
  }
}
