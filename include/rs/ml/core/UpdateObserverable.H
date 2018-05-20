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

#include <rs/ml/core/UpdateObserver.H>
#include <boost/range/algorithm/for_each.hpp>
#include <set>

namespace rs::ml::core
{
  template<typename... Contexts>
  class UpdateObserverable
  {
  public:
    void attach(UpdateObserver<Contexts...>& observer) { _observers.insert(&observer); observer.onAttached(); }
    void detach(UpdateObserver<Contexts...>& observer) { _observers.erase(&observer); observer.onDetached(); }
    void beginUpdate() { boost::for_each(_observers, [](auto ob) { ob->onBeginUpdate(); }); }
    void endUpdate() { boost::for_each(_observers, [](auto ob) { ob->onEndUpdate(); }); }
    void create(Contexts... ctxts) { boost::for_each(_observers, [&](auto ob) { ob->onCreate(ctxts...); }); }
    void modify(Contexts... ctxts) { boost::for_each(_observers, [&](auto ob) { ob->onModify(ctxts...); }); }
    void remove(Contexts... ctxts) { boost::for_each(_observers, [&](auto ob) { ob->onRemove(ctxts...); }); }

  private:
    std::set<UpdateObserver<Contexts...>*> _observers;
  };
}

