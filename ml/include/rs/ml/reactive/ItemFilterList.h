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
#include <functional>
#include <iostream>
#include <QtCore/QDebug>

namespace rs::ml::reactive
{
  template<typename T>
  class ItemFilterList : public AbstractItemList<T>, public Observer<const T&, utility::TaggedIndex<struct IndexTag>>
  {
  public:
    using Id = typename T::Id;
    using Index = typename AbstractItemList<T>::Index;
    using Observer = rs::ml::reactive::Observer<const T&, utility::TaggedIndex<struct IndexTag>>;
    using Filter = std::function<bool(const T&)>;

    ItemFilterList(AbstractItemList<T>& source, const Filter& filter) : _source{source}, _filter{filter} 
    {
      for (auto i = 0u; i < _source.size(); ++i)
      {
        const auto& t = source.at(Index{i});

        if (!_filter || _filter(t))
        {
          _items.emplace_hint(_items.end(), t.id, Index{i});
        }
      }

      source.attach(*this);
    }

    std::size_t size() const override { return _items.size(); }
    const T& at(Index index) const override { return _source.at(_items.nth(index)->second); }

    void attach(Observer& observer) override { _observerable.attach(observer); }
    void detach(Observer& observer) override { _observerable.detach(observer); }

  protected:
    void onInsert(const T& t, Index index) override
    {
      if (!_filter || _filter(t))
      {
        insert(t, index);
      }
    };

    void onUpdate(const T& t, Index index) override
    {
      auto iter = _items.find(t.id);

      if (!_filter || _filter(t))
      {
        if (iter == _items.end())
        {
          insert(t, index);
        }
        else
        {
          update(t, iter);
        }
      }
      else if (iter != _items.end())
      {
        remove(t, iter);
      }
    };

    void onRemove(const T& t, Index) override
    {
      auto iter = _items.find(t.id);

      if (iter != _items.end())
      {
        remove(t, iter);
      }
    };

  private:
/*     struct Compare
    {
      using is_transparent = void;
      bool operator()(Id a, Id b) const { return a < b; }
      // bool operator()(const T* a, Id id) const { return a->id < id; }
      // bool operator()(Id id, const T* a) const { return id < a->id; }
    }; */

    using Container = boost::container::flat_map<Id, Index>;

    void insert(const T& t, Index index)
    {
      // qInfo() << "before insert " << t.id << " " << i << " " << &(t);

      auto iter = _items.emplace_hint(_items.end(), t.id, index);
      _observerable.insert(t, Index{_items.index_of(iter)});
      // qInfo() << "after insert" << (_items.index_of(iter)) << " " << *iter;
    }

    void update(const T& t, typename Container::iterator iter)
    {
      _observerable.update(t, Index{_items.index_of(iter)});
    }

    void remove(const T& t, typename Container::iterator iter)
    {
      std::size_t index = _items.index_of(iter);
      _observerable.remove(t, Index{index});
      _items.erase(iter);
    }

    AbstractItemList<T>& _source;
    Filter _filter;
    Container _items;
    Observerable<const T&, Index> _observerable;
  };
}
