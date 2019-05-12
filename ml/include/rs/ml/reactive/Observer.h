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

namespace rs::ml::reactive
{
  template<typename... Contexts>
  class Observerable;

  template<typename... Contexts>
  class Observer
  {
  public:
    virtual ~Observer() = default;

  protected:
    virtual void onAttached() {};
    virtual void onInsert(Contexts...) {};
    virtual void onUpdate(Contexts...) {};
    virtual void onRemove(Contexts...) {};
    virtual void onDetached() {};

    friend class Observerable<Contexts...>;
  };
}
