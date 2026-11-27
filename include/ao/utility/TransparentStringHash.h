// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstddef>
#include <functional>
#include <string_view>

namespace ao::utility
{
  // Pass a view for length-delimited keys (including embedded nulls). A raw
  // character pointer, if used, must name a non-null, null-terminated C string.
  struct TransparentStringHash final
  {
    using is_transparent = void;

    std::size_t operator()(std::string_view value) const noexcept { return std::hash<std::string_view>{}(value); }
  };

  struct TransparentStringEqual final
  {
    using is_transparent = void;

    bool operator()(std::string_view lhs, std::string_view rhs) const noexcept { return lhs == rhs; }
  };
} // namespace ao::utility
