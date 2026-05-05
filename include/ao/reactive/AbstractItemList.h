// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/utility/TaggedInteger.h>

#include <algorithm>
#include <vector>

namespace ao::reactive
{
  template<typename Id, typename T>
  class AbstractItemList
  {
    template<typename N>
    struct IndexTag
    {};

  public:
    using Value = std::pair<Id, T>;
    using Index = utility::TaggedIndex<IndexTag<T>>;
    class Observer;

    virtual ~AbstractItemList() = default;

    virtual std::size_t size() const = 0;

    virtual Value const& at(Index idx) const = 0;

    virtual void attach(Observer& observer) = 0;

    virtual void detach(Observer& observer) = 0;

  protected:
    class Observerable;
  };

  template<typename Id, typename T>
  class AbstractItemList<Id, T>::Observer
  {
  public:
    virtual ~Observer() = default;

  protected:
    virtual void onAttached() {};

    virtual void onBeginInsert(Id, Index){};

    virtual void onEndInsert(Id, T const&, Index) {};

    virtual void onBeginUpdate(Id, T const&, Index) {};

    virtual void onEndUpdate(Id, T const&, Index) {};

    virtual void onBeginRemove(Id, T const&, Index) {};

    virtual void onEndRemove(Id id, Index) {};

    virtual void onBeginClear() {};

    virtual void onEndClear() {};

    virtual void onDetached() {};

    friend class AbstractItemList<Id, T>::Observerable;
  };

  template<typename Id, typename T>
  class AbstractItemList<Id, T>::Observerable
  {
  public:
    using Observer = AbstractItemList<Id, T>::Observer;

    void attach(Observer& observer)
    {
      _observers.push_back(&observer);
      observer.onAttached();
    }

    void detach(Observer& observer)
    {
      std::erase(_observers, &observer);
      observer.onDetached();
    }

    void beginInsert(Id id, Index idx) { forAll(std::mem_fn(&Observer::onBeginInsert), id, idx); }

    void endInsert(Id id, T const& val, Index idx) { forAll(std::mem_fn(&Observer::onEndInsert), id, val, idx); }

    void beginUpdate(Id id, T const& val, Index idx) { forAll(std::mem_fn(&Observer::onBeginUpdate), id, val, idx); }

    void endUpdate(Id id, T const& val, Index idx) { forAll(std::mem_fn(&Observer::onEndUpdate), id, val, idx); }

    void beginRemove(Id id, T const& val, Index idx) { forAll(std::mem_fn(&Observer::onBeginRemove), id, val, idx); }

    void endRemove(Id id, Index idx) { forAll(std::mem_fn(&Observer::onEndRemove), id, idx); }

    void beginClear() { forAll(std::mem_fn(&Observer::onBeginClear)); }

    void endClear() { forAll(std::mem_fn(&Observer::onEndClear)); }

  private:
    template<typename Memfun, typename... Args>
    void forAll(Memfun memfun, Args&&... args)
    {
      for (auto* ob : _observers)
      {
        std::invoke(memfun, ob, std::forward<Args>(args)...);
      }
    }

    std::vector<Observer*> _observers;
  };
}
