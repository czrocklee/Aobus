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

#include <rs/reactive/AbstractItemList.h>

#include <boost/container/flat_map.hpp>

namespace rs::reactive
{
  template<typename Id, typename T>
  class ItemList : public AbstractItemList<Id, T>
  {
  public:
    using Value = AbstractItemList<Id, T>::Value;
    using Index = AbstractItemList<Id, T>::Index;
    using Observer = AbstractItemList<Id, T>::Observer;

    std::size_t size() const override { return _items.size(); }

    const Value& at(Index idx) const override { return *_items.nth(idx); }

    void attach(Observer& observer) override { _observerable.attach(observer); }

    void detach(Observer& observer) override { _observerable.detach(observer); }

    template<typename... Args>
    T& insert(Id id, Args&&... args)
    {
      auto iterHint = (!_items.empty() && _items.rbegin()->first < id) ? _items.end() : _items.lower_bound(id);
      auto idx = Index{_items.index_of(iterHint)};
      _observerable.beginInsert(id, idx);
      auto iter = _items.emplace_hint(iterHint, id, std::forward<Args>(args)...);
      _observerable.endInsert(id, iter->second, idx);
      return iter->second;
    }

    template<typename F>
    void update(Id id, F&& f)
    {
      if (auto iter = _items.find(id); iter != _items.end())
      {
        auto idx = Index{_items.index_of(iter)};
        _observerable.beginUpdate(id, iter->second, idx);
        std::invoke(std::forward<F>(f), iter->second);
        _observerable.endUpdate(id, iter->second, idx);
      }
    }

    void remove(Id id)
    {
      if (auto iter = _items.find(id); iter != _items.end())
      {
        auto idx = Index{_items.index_of(iter)};
        _observerable.beforeRemove(id, iter->second, idx);
        _items.erase(id);
        _observerable.endRemove(id, idx);
      }
    }

    void clear()
    {
      _observerable.beginClear();
      _items.clear();
      _observerable.endClear();
    }

  private:
    /*     struct Compare
        {
          using is_transparent = void;
          bool operator()(const T& a, const T& b) const { return a.id < b.id; }
          bool operator()(const T& a, Id id) const { return a.id < id; }
        }; */

    boost::container::flat_map<Id, T> _items;
    AbstractItemList<Id, T>::Observerable _observerable;
  };
}
