#include <algorithm>
#include <cstdint>
#include <vector>

void positiveMinElementDeref()
{
  auto v = std::vector<int>{1, 2, 3};
  // POSITIVE
  auto minVal = *std::min_element(v.begin(), v.end());
  (void)minVal;
}

void positiveMaxElementDeref()
{
  auto v = std::vector<int>{1, 2, 3};
  // POSITIVE
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
  // POSITIVE
  auto minVal = *std::min_element(v.begin(), v.end(), less);
  (void)minVal;
}

void negativeNoDeref()
{
  auto v = std::vector<int>{1, 2, 3};
  auto it = std::min_element(v.begin(), v.end());
  // NEGATIVE: no dereference — user wants the iterator
  (void)it;
}

void negativePartialRange()
{
  auto v = std::vector<int>{1, 2, 3, 4, 5};
  // NEGATIVE
  auto minVal = *std::min_element(v.begin() + 1, v.end() - 1);
  (void)minVal;
}
