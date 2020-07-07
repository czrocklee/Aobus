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

#include "FrameLayout.h"
#include <string_view>
#include <vector>
#include <memory>
#include <functional>
#include <numeric>
#include <cassert>

namespace rs::tag::mpeg
{
  class Frame
  {
  public:
    std::size_t length() const;
    bool isValid() const;
   
  private:
    FrameLayout _layout;
  };

  const Frame* locate(const void* buffer, std::size_t size);
}
