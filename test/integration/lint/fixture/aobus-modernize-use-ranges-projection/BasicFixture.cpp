// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>

struct Item
{
  std::int32_t id;
  bool isValid() const { return id > 0; }
};

#define AOBUS_IS_VALID_PRED [](Item const& item) { return item.isValid(); }

void testProjections(std::vector<Item>& v)
{
  // POSITIVE: FIX-TO: bool const anyValid = std::ranges::any_of(v, &Item::isValid);
  bool const anyValid = std::ranges::any_of(v, [](Item const& item) { return item.isValid(); });

  auto ids = std::vector<int>{};
  // POSITIVE: FIX-TO: std::ranges::transform(v, std::back_inserter(ids), &Item::id);
  std::ranges::transform(v, std::back_inserter(ids), [](Item const& item) { return item.id; });

  // POSITIVE: FIX-TO: std::ranges::sort(v, {}, &Item::id);
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

  // NEGATIVE - generic lambda: the parameter type cannot be spelled in a projection
  std::ranges::any_of(v, [](auto const& item) { return item.isValid(); });

  // NEGATIVE - lambda spelled inside a macro expansion
  std::ranges::any_of(v, AOBUS_IS_VALID_PRED);
}
