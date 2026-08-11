// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/utility/ByteView.h>

#include <cstddef>
#include <span>
#include <string_view>

namespace ao::utility::bytes
{
  std::string_view stringView(std::span<std::byte const> span) noexcept
  {
    if (span.empty())
    {
      return {};
    }

    auto const* const data = layout::view<char>(span);
    return {data, span.size()};
  }
} // namespace ao::utility::bytes
