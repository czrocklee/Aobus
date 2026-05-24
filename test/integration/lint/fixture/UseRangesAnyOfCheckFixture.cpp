#include "TestHelpers.h"

#include <algorithm>
#include <vector>

void testAnyAll(std::vector<int> const& v)
{
  auto pred = [](int x) { return x > 0; };

  // POSITIVE
  if (std::ranges::find_if(v, pred) != v.end())
  {
  }

  // POSITIVE
  if (std::ranges::find_if(v, [](int x) { return x > 0; }) != std::ranges::end(v))
  {
  }

  // POSITIVE
  bool hasPositive = std::ranges::find_if(v, pred) != std::end(v);

  // NEGATIVE
  if (std::ranges::any_of(v, pred))
  {
  }

  // POSITIVE
  if (std::ranges::find_if(v, pred) == v.end())
  {
  }

  // POSITIVE
  if (std::ranges::find_if_not(v, pred) == v.end())
  {
  }

  // NEGATIVE
  auto it = std::ranges::find_if(v, pred);
  if (it != v.end())
  {
    // using iterator, should not be converted
  }
}
