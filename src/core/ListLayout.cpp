// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/core/ListLayout.h>

namespace rs::core
{

  std::string_view ListView::getString(std::uint16_t offset, std::uint16_t length) const  // NOLINT(bugprone-easily-swappable-parameters)
  {
    if (length == 0) {
      return {};
    }

    std::size_t const start = sizeof(ListHeader) + offset;
    if (start + length > _size)
    {
      return {};
    }

    return {utility::as<char>(_header, start), length};
  }

} // namespace rs::core
