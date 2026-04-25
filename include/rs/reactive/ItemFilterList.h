// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/reactive/AbstractItemList.h>

#include <boost/container/flat_map.hpp>
#include <functional>
#include <ranges>

namespace rs::reactive
{
  template<typename Id, typename T>
  class ItemFilterList
    : public AbstractItemList<Id, T>
    , public AbstractItemList<Id, T>::Observer
  {
  public:
    using Value = AbstractItemList<Id, T>::Value;
    using Index = AbstractItemList<Id, T>::Index;
    using Observer = AbstractItemList<Id, T>::Observer;
    using Filter = std::move_only_function<bool(T const&)>;

    ItemFilterList(AbstractItemList<Id, T>& source, Filter filter)
      : _source{source}, _filter{std::move(filter)}
    {
      for (auto const idx : std::views::iota(0u, _source.size()))
      {
        auto const& [id, t] = _source.at(Index{idx});

        if (!_filter || _filter(t))
        {
          _items.emplace_hint(_items.end(), id, Index{idx});
        }
      }

      source.attach(*this);
    }

    std::size_t size() const override { return _items.size(); }

    Value const& at(Index idx) const override { return _source.at(_items.nth(idx)->second); }

    void attach(Observer& observer) override { _observerable.attach(observer); }

    void detach(Observer& observer) override { _observerable.detach(observer); }

  protected:
    void onEndInsert(Id id, T const& val, Index idx) override
    {
      if (!_filter || _filter(val))
      {
        insert(id, val, idx);
      }
    
    };

    void onBeginUpdate(Id id, T const& val, Index idx) override { onBeginRemove(id, val, idx); };

    void onEndUpdate(Id id, T const& val, Index idx) { onEndInsert(id, val, idx); }

    void onBeginRemove(Id id, T const& val, Index) override
    {
      if (auto iter = _items.find(id); iter != _items.end())
      {
        remove(id, val, iter);
      }
    
    };

    void onBeginClear() override
    {
      _observerable.beginClear();
      _items.clear();
      _observerable.endClear();
    }

  private:
    /*     struct Compare
        {
          using is_transparent = void;
          bool operator()(Id a, Id b) const { return a < b; }
          // bool operator()(const T* a, Id id) const { return a->id < id; }
          // bool operator()(Id id, const T* a) const { return id < a->id; }
        }; */

    using Container = boost::container::flat_map<Id, Index>;

    void insert(Id id, T const& val, Index idx)
    {
      auto iterHint = (!_items.empty() && _items.rbegin()->first < id) ? _items.end() : _items.lower_bound(id);
      auto localIdx = Index{_items.index_of(iterHint)};
      _observerable.beginInsert(id, localIdx);
      _items.emplace_hint(iterHint, id, idx);
      _observerable.endInsert(id, val, localIdx);
    }

    void remove(Id id, T const& val, typename Container::iterator iter)
    {
      auto idx = Index{_items.index_of(iter)};
      _observerable.beginRemove(id, val, idx);
      _items.erase(iter);
      _observerable.endRemove(id, idx);
    }

    AbstractItemList<Id, T>& _source;
    Filter _filter;
    Container _items;
    AbstractItemList<Id, T>::Observerable _observerable;
  };
}
