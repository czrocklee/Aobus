/*
 * Copyright (C) <year> <name of author>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of  MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <rs/ml/utility/TaggedInteger.h>

namespace rs::ml::core
{
  template<typename T>
  struct Item
  {
    using Id = utility::TaggedInteger<std::uint64_t, struct IdTag>;
    using Value = T;

    Id id;
    const Value* value;
  };

  template<typename T>
  struct ItemT
  {
    using Id = typename Item<T>::Id;
    using Value = typename T::NativeTableType;

    Id id;
    Value value;

    static ItemT fromItem(const Item<T>& t)
    {
      ItemT item{t.id};
      t.value->UnPackTo(&item.value);
      return item;
    }
  };

}
