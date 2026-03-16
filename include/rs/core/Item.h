// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/utility/TaggedInteger.h>

namespace rs::core
{
  template<typename T>
  struct Item
  {
    using Id = utility::TaggedInteger<std::uint32_t, struct IdTag>;
    using Value = T;

    Id id;
    Value const* value;
  };

  template<typename T>
  struct ItemT
  {
    using Id = typename Item<T>::Id;
    using Value = typename T::NativeTableType;

    Id id;
    Value value;

    static ItemT fromItem(Item<T> const& t)
    {
      ItemT item{t.id};
      t.value->UnPackTo(&item.value);
      return item;
    }
  };

}
