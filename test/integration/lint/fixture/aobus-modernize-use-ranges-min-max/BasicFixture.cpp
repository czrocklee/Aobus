// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <algorithm>
#include <cstdint>
#include <vector>

void positiveMinElementDeref()
{
  auto v = std::vector<int>{1, 2, 3};
  // POSITIVE: FIX-TO: auto minVal = std::ranges::min(v);
  auto minVal = *std::min_element(v.begin(), v.end());
  (void)minVal;
}

void positiveMaxElementDeref()
{
  auto v = std::vector<int>{1, 2, 3};
  // POSITIVE: FIX-TO: auto maxVal = std::ranges::max(v);
  auto maxVal = *std::max_element(v.begin(), v.end());
  (void)maxVal;
}

bool less(std::int32_t a, std::int32_t b)
{
  return a < b;
}

void positiveMinElementWithComp()
{
  auto v = std::vector<int>{1, 2, 3};
  // POSITIVE: FIX-TO: auto minVal = std::ranges::min(v, less);
  auto minVal = *std::min_element(v.begin(), v.end(), less);
  (void)minVal;
}

void negativeNoDeref()
{
  auto v = std::vector<int>{1, 2, 3};
  auto it = std::min_element(v.begin(), v.end());
  // NEGATIVE - no dereference — user wants the iterator
  (void)it;
}

void negativePartialRange()
{
  auto v = std::vector<int>{1, 2, 3, 4, 5};
  // NEGATIVE
  auto minVal = *std::min_element(v.begin() + 1, v.end() - 1);
  (void)minVal;
}

namespace mylib
{
  // Same unqualified name as std::min_element, different semantics.
  std::vector<int>::const_iterator min_element(std::vector<int>::const_iterator first,
                                               std::vector<int>::const_iterator last);
} // namespace mylib

#define AOBUS_MIN_OF(c) (*std::min_element(c.begin(), c.end()))

void negativeCustomMinElement()
{
  auto v = std::vector<int>{1, 2, 3};
  // NEGATIVE - same name, but not std::min_element
  auto minVal = *mylib::min_element(v.begin(), v.end());
  (void)minVal;
}

void negativeCrossContainer(std::vector<int> const& v, std::vector<int> const& w)
{
  // NEGATIVE - begin() and end() come from different containers
  auto minVal = *std::min_element(v.begin(), w.end());
  (void)minVal;
}

void negativeMacroSpelled()
{
  auto v = std::vector<int>{1, 2, 3};
  // NEGATIVE - the dereferenced min_element is spelled inside a macro expansion
  auto minVal = AOBUS_MIN_OF(v);
  (void)minVal;
}
