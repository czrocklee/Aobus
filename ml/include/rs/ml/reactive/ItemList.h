/*
 * Copyright (C) <year> <name of author>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of  MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <rs/ml/reactive/AbstractItemList.h>
#include <rs/ml/reactive/Observerable.h>

#include <boost/container/flat_map.hpp>
#include <iostream>
namespace rs::ml::reactive
{
  template<typename Id, typename T>
  class ItemList : public AbstractItemList<Id, T>
  {
  public:
    using Value = typename AbstractItemList<Id, T>::Value;
    using Index = typename AbstractItemList<Id, T>::Index;
    using Observer = typename AbstractItemList<Id, T>::Observer;

    std::size_t size() const override { return _items.size(); }
    const Value& at(Index index) const override { return *_items.nth(index); }

    void attach(Observer& observer) override { _observerable.attach(observer); }
    void detach(Observer& observer) override { _observerable.detach(observer); }

    template<typename... Args>
    Value& insert(Id id, Args&&... args)
    {
      auto iter = _items.emplace_hint(_items.end(), id, std::forward<Args>(args)...);
      _observerable.insert(iter->first, iter->second, Index{_items.index_of(iter)});
      return *iter;
    }

    template<typename F>
    void update(Id id, F&& f)
    {
      if (auto iter = _items.find(id); iter != _items.end())
      {
        std::invoke(std::forward<F>(f), iter->second);
        _observerable.update(id, iter->second, Index{_items.index_of(iter)});
      }
    }

    void erase(Id id)
    {
      if (auto iter = _items.find(id); iter != _items.end())
      {
        auto index = _items.index_of(iter);
        _observerable.erase(id, iter->second, Index{index});
        _items.erase(id);
      }
    }

  private:
/*     struct Compare
    {
      using is_transparent = void;
      bool operator()(const T& a, const T& b) const { return a.id < b.id; }
      bool operator()(const T& a, Id id) const { return a.id < id; }
    }; */

    boost::container::flat_map<Id, T> _items;
    Observerable<Id, const T&, Index> _observerable;
  };
}
