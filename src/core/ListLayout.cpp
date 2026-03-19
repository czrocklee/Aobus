// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/core/ListLayout.h>

namespace rs::core
{

  std::string_view ListView::getString(std::uint16_t offset, std::uint16_t len) const
  {
    if (len == 0) return {};

    std::size_t const start = sizeof(ListHeader) + offset;
    if (start + len > _size)
    {
      return {};
    }

    return {utility::as<char>(_header, start), len};
  }

} // namespace rs::core
