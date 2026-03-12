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

namespace rs::reactive
{
  template<typename Id, typename... Args>
  class Observerable;

  template<typename Id, typename... Args>
  class Observer
  {
  public:
    virtual ~Observer() = default;

  protected:
    virtual void onAttached() {};
    virtual void onBeginInsert(Id id) {};
    virtual void onEndInsert(Id id, Args&&...) {};
    virtual void onBeginUpdate(Id id, Args&&...) {};
    virtual void onEndUpdate(Id id, Args&&...) {};
    virtual void onBeginRemove(Id id, Args&&...) {};
    virtual void onEndRemove(Id id) {};
    virtual void onBeginClear() {};
    virtual void onEndClear() {};
    virtual void onDetached() {};

    friend class Observerable<Args...>;
  };
}
