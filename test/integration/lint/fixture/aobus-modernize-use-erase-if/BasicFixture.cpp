// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <algorithm>
#include <cstdint>
#include <vector>

bool isNegative(std::int32_t x)
{
  return x < 0;
}

void positiveRemoveIf()
{
  auto v = std::vector<int>{1, 2, 3};
  // POSITIVE
  v.erase(std::remove_if(v.begin(), v.end(), isNegative), v.end());
}

void positiveRemove()
{
  auto v = std::vector<int>{1, 2, 3};
  // POSITIVE
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
