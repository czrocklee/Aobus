// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace ao::utility
{
  struct TransparentStringHash final
  {
    using is_transparent = void; // NOLINT(readability-identifier-naming)

    std::size_t operator()(std::string_view value) const noexcept { return std::hash<std::string_view>{}(value); }

    std::size_t operator()(std::string const& value) const noexcept { return (*this)(std::string_view{value}); }
  };

  struct TransparentStringEqual final
  {
    using is_transparent = void; // NOLINT(readability-identifier-naming)

    bool operator()(std::string_view lhs, std::string_view rhs) const noexcept { return lhs == rhs; }
  };
} // namespace ao::utility
