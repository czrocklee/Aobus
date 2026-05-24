#include "TestHelpers.h"

#include <algorithm>
#include <vector>

struct Item
{
  int id;
  bool isValid() const { return id > 0; }
};

void testProjections(std::vector<Item>& v)
{
  // POSITIVE
  bool anyValid = std::ranges::any_of(v, [](Item const& item) { return item.isValid(); });

  std::vector<int> ids;
  // POSITIVE
  std::ranges::transform(v, std::back_inserter(ids), [](Item const& item) { return item.id; });

  // POSITIVE
  std::ranges::sort(v, [](Item const& a, Item const& b) { return a.id < b.id; });

  // NEGATIVE
  std::ranges::any_of(v,
                      [](Item const& item)
                      {
                        if (item.id == 0) return false;
                        return item.isValid();
                      });

  // NEGATIVE
  std::ranges::any_of(v, [](Item const& item) { return item.isValid() && item.id > 10; });
}
