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

#include <rs/utility/TaggedInteger.h>

#include <vector>

namespace rs::reactive
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

    [[nodiscard]] virtual std::size_t size() const = 0;

    [[nodiscard]] virtual const Value& at(Index idx) const = 0;

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

    virtual void onEndInsert(Id, const T&, Index) {};

    virtual void onBeginUpdate(Id, const T&, Index) {};

    virtual void onEndUpdate(Id, const T&, Index) {};

    virtual void onBeginRemove(Id, const T&, Index) {};

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
      _observers.erase(std::find(_observers.begin(), _observers.end(), &observer));
      observer.onDetached();
    }

    void beginInsert(Id id, Index idx) { forAll(std::mem_fn(&Observer::onBeginInsert), id, idx); }

    void endInsert(Id id, const T& val, Index idx) { forAll(std::mem_fn(&Observer::onEndInsert), id, val, idx); }

    void beginUpdate(Id id, const T& val, Index idx) { forAll(std::mem_fn(&Observer::onBeginUpdate), id, val, idx); }

    void endUpdate(Id id, const T& val, Index idx) { forAll(std::mem_fn(&Observer::onEndUpdate), id, val, idx); }

    void beginRemove(Id id, const T& val, Index idx) { forAll(std::mem_fn(&Observer::onBeginRemove), id, val, idx); }

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
