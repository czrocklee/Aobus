// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/core/ListLayout.h>

namespace rs::core
{

  std::string_view ListView::getString(std::uint16_t offset, std::uint16_t length) const
  {
    if (length == 0) {
      return {};
    }

    auto const start = kListHeaderSize + offset;
    if (start + length > _size)
    {
      return {};
    }

    return {reinterpret_cast<char const*>(_payload.data()) + start, length};
  }

  std::string_view ListView::name() const
  {
    if (!isValid()) return {};
    return getString(header()->nameOffset, header()->nameLen);
  }

  std::string_view ListView::description() const
  {
    if (!isValid()) return {};
    return getString(header()->descOffset, header()->descLen);
  }

  std::string_view ListView::filter() const
  {
    if (!isValid()) return {};
    return getString(header()->filterOffset, header()->filterLen);
  }

  std::span<TrackId const> ListView::trackIds() const
  {
    if (!isValid()) {
      return {};
    }

    auto const offset = kListHeaderSize;
    auto const count = static_cast<std::size_t>(header()->trackIdsCount);

    if (offset + count * sizeof(TrackId) > _size) {
      return {};
    }

    auto const* ptr = reinterpret_cast<TrackId const*>(_payload.data() + offset);
    return {ptr, count};
  }

} // namespace rs::core
