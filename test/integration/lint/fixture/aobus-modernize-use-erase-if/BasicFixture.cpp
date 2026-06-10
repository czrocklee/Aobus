// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <algorithm>
#include <cstdint>
#include <vector>

bool isNegative(std::int32_t x)
{
  return x < 0;
}

namespace mylib
{
  // Same unqualified name as std::remove_if, different semantics.
  std::vector<int>::iterator remove_if(std::vector<int>::iterator first,
                                       std::vector<int>::iterator last,
                                       bool (*pred)(std::int32_t));
} // namespace mylib

#define AOBUS_PURGE(c, pred) c.erase(std::remove_if(c.begin(), c.end(), pred), c.end())

void positiveRemoveIf()
{
  auto v = std::vector<int>{1, 2, 3};
  // POSITIVE: FIX-TO: std::erase_if(v, isNegative);
  v.erase(std::remove_if(v.begin(), v.end(), isNegative), v.end());
}

void positiveRemove()
{
  auto v = std::vector<int>{1, 2, 3};
  // POSITIVE: FIX-TO: std::erase(v, 5);
  v.erase(std::remove(v.begin(), v.end(), 5), v.end());
}

void negativePartialRange()
{
  auto v = std::vector<int>{1, 2, 3, 4, 5};
  // NEGATIVE
  v.erase(std::remove_if(v.begin() + 1, v.end() - 1, isNegative), v.end());
}

void negativeNonEraseRemove()
{
  auto v = std::vector<int>{1, 2, 3};
  auto it = std::remove_if(v.begin(), v.end(), isNegative);
  // NEGATIVE - no erase, just remove_if alone
}

void negativeCustomRemoveIf()
{
  auto v = std::vector<int>{1, 2, 3};
  // NEGATIVE - same name, but not std::remove_if
  v.erase(mylib::remove_if(v.begin(), v.end(), isNegative), v.end());
}

void negativeMacroSpelled()
{
  auto v = std::vector<int>{1, 2, 3};
  // NEGATIVE - the erase-remove idiom is spelled inside a macro expansion
  AOBUS_PURGE(v, isNegative);
}
