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
#include <cassert>
#include <functional>
#include <memory>
#include <numeric>
#include <optional>
#include <string_view>
#include <vector>

namespace rs::tag::mpeg
{
  class FrameView
  {
  public:
    FrameView(void const* data, std::size_t size) : _data{data}, _size{size} {}

    std::size_t length() const;

    bool isValid() const;

    FrameLayout const& layout() const { return *static_cast<FrameLayout const*>(_data); }

  private:
    void const* _data;
    std::size_t _size;
  };

  std::optional<FrameView> locate(void const* buffer, std::size_t size);
}
