#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>

struct Item
{
  std::int32_t id;
  bool isValid() const { return id > 0; }
};

void testProjections(std::vector<Item>& v)
{
  // POSITIVE
  bool const anyValid = std::ranges::any_of(v, [](Item const& item) { return item.isValid(); });

  auto ids = std::vector<int>{};
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
