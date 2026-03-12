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

#include <rs/reactive/Observer.h>
#include <vector>

namespace rs::reactive
{
  template<typename Id, typename... Args>
  class Observerable
  {
  public:
    using ObserverT = Observer<Id, Args...>;

    void attach(ObserverT& observer)
    {
      _observers.push_back(&observer);
      observer.onAttached();
    }

    void detach(ObserverT& observer)
    {
      _observers.erase(std::find(_observers.begin(), _observers.end(), &observer));
      observer.onDetached();
    }

    void beforeInsert(Id id) { forAll(std::mem_fn(&ObserverT::onBeforeInsert), id); }

    void endInsert(Id id, Args... args)
    {
      forAll(std::mem_fn(&ObserverT::onEndInsert), id, std::forward<Args>(args)...);
    }

    void beginUpdate(Id id, Args... args)
    {
      forAll(std::mem_fn(&ObserverT::onBeginUpdate), id, std::forward<Args>(args)...);
    }

    void endUpdate(Id id, Args... args)
    {
      forAll(std::mem_fn(&ObserverT::onEndUpdate), id, std::forward<Args>(args)...);
    }

    void beginRemove(Id id, Args... args)
    {
      forAll(std::mem_fn(&ObserverT::onBeginRemove), id, std::forward<Args>(args)...);
    }

    void endRemove(Id id) { forAll(std::mem_fn(&ObserverT::onEndRemove), id); }

    void beginClear() { forAll(std::mem_fn(&ObserverT::onBeginClear)); }

    void endClear() { forAll(std::mem_fn(&ObserverT::onEndClear)); }

  private:
    template<typename Memfun>
    void forAll(Memfun memfun, Args... args)
    {
      for (auto* ob : _observers) { std::invoke(memfun, ob, std::forward<Args>(args)...); }
    }

    std::vector<ObserverT*> _observers;
  };
}
