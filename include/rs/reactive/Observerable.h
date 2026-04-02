// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

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
      std::erase(_observers, &observer);
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
      for (auto* ob : _observers)
      {
        std::invoke(memfun, ob, std::forward<Args>(args)...);
      }
    }

    std::vector<ObserverT*> _observers;
  };
}
