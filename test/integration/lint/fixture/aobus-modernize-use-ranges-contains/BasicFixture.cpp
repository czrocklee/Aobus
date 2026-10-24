// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <algorithm>
#include <array>
#include <cstdint>
#include <ranges>
#include <vector>

// Truth of the recommended predicate. Stored results are not call-comparisons,
// so these asserts are not diagnostics.
namespace
{
  constexpr auto kHaystack = std::to_array<int>({1, 2, 5});
  constexpr auto kPresent = std::ranges::find(kHaystack, 5);
  constexpr auto kAbsent = std::ranges::find(kHaystack, 4);
  constexpr auto kEnd = std::ranges::end(kHaystack);
  constexpr auto kPresentCount = std::ranges::count(kHaystack, 5);
  constexpr auto kAbsentCount = std::ranges::count(kHaystack, 4);

  static_assert(std::ranges::contains(kHaystack, 5));
  static_assert(!std::ranges::contains(kHaystack, 4));
  static_assert((kPresent != kEnd) == std::ranges::contains(kHaystack, 5));
  static_assert((kAbsent == kEnd) == !std::ranges::contains(kHaystack, 4));
  static_assert((kPresentCount > 0) == std::ranges::contains(kHaystack, 5));
  static_assert((kPresentCount != 0) == std::ranges::contains(kHaystack, 5));
  static_assert((kAbsentCount == 0) == !std::ranges::contains(kHaystack, 4));
  static_assert((0 == kAbsentCount) == !std::ranges::contains(kHaystack, 4));
  static_assert((0 != kPresentCount) == std::ranges::contains(kHaystack, 5));
  // 0 > count is always false and disagrees with contains when the value is present.
  static_assert(!(0 > kPresentCount));
  static_assert((0 > kPresentCount) != std::ranges::contains(kHaystack, 5));
  // 0 < count matches contains, but the checker does not diagnose that spelling.
  static_assert((0 < kPresentCount) == std::ranges::contains(kHaystack, 5));
} // namespace

// Not an end() accessor even though its name contains "end".
std::vector<int>::const_iterator legend(std::vector<int> const& v);

namespace mylib
{
  // Same unqualified name as std::ranges::find, different semantics.
  std::vector<int>::const_iterator find(std::vector<int> const& v, int val);
} // namespace mylib

#define AOBUS_COUNT(c, x) std::ranges::count(c, x)

// Argument-list macro: each argument covers part of the expansion while the
// comparison stays file-spelled. Count does not need range-text identity.
#define AOBUS_COUNT_ARGS v, 6

void testFind(std::vector<int> const& v, std::vector<int> const& w)
{
  // POSITIVE | use std::ranges::contains instead of std::ranges::find != end
  if (std::ranges::find(v, 5) != v.end())
  {
  }

  // POSITIVE | use !std::ranges::contains instead of std::ranges::find == end
  if (std::ranges::find(v, 6) == std::ranges::end(v))
  {
  }

  // POSITIVE | use std::ranges::contains instead of std::ranges::find != end
  bool const found = std::ranges::find(v, 7) != std::end(v);

  // POSITIVE | use std::ranges::contains instead of std::ranges::find != end
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
  // POSITIVE | use std::ranges::contains instead of std::ranges::count > 0
  if (std::ranges::count(v, 5) > 0)
  {
  }

  // POSITIVE | use std::ranges::contains instead of std::ranges::count != 0
  if (std::ranges::count(v, 6) != 0)
  {
  }

  // POSITIVE | use !std::ranges::contains instead of std::ranges::count == 0
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

void testReversedComparisons(std::vector<int> const& v)
{
  // POSITIVE | use !std::ranges::contains instead of std::ranges::find == end
  if (std::ranges::end(v) == std::ranges::find(v, 5))
  {
  }

  // POSITIVE | use !std::ranges::contains instead of std::ranges::count == 0
  if (0 == std::ranges::count(v, 6))
  {
  }

  // POSITIVE | use std::ranges::contains instead of std::ranges::count != 0
  if (0 != std::ranges::count(v, 7))
  {
  }

  // NEGATIVE - 0 > count is always false and is not a contains test
  if (0 > std::ranges::count(v, 8))
  {
  }

  // NEGATIVE - reversed < is not matched; do not treat it as count > 0
  if (0 < std::ranges::count(v, 9))
  {
  }
}

void positiveCountPartialMacroArgs(std::vector<int> const& v)
{
  // POSITIVE | use !std::ranges::contains instead of std::ranges::count == 0
  if (std::ranges::count(AOBUS_COUNT_ARGS) == 0)
  {
  }
}
