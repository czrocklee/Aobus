// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

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
