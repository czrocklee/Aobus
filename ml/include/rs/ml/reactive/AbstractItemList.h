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

#include <rs/common/utility/TaggedInteger.h>
#include <rs/ml/reactive/Observerable.h>

namespace rs::ml::reactive
{
  template<typename Id, typename T>
  class AbstractItemList
  {
    template<typename N>
    struct IndexTag
    {};

  public:
    using Value = std::pair<Id, T>;
    using Index = common::utility::TaggedIndex<IndexTag<T>>;
    using Observer = rs::ml::reactive::Observer<Id, const T&, Index>;

    virtual ~AbstractItemList() = default;

    virtual std::size_t size() const = 0;
    virtual const Value& at(Index index) const = 0;

    virtual void attach(Observer& observer) = 0;
    virtual void detach(Observer& observer) = 0;
  };
}
