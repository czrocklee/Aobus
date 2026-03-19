// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/core/TrackLayout.h>

#include <cstring>

namespace rs::core
{

  // TrackView::TagProxy implementation
  std::vector<DictionaryId> TrackView::TagProxy::ids() const
  {
    std::vector<DictionaryId> result;
    auto c = count();
    for (std::uint8_t i = 0; i < c; ++i)
    {
      auto tagId = id(i);
      if (tagId > 0)
      {
        result.push_back(tagId);
      }
    }
    return result;
  }

  bool TrackView::TagProxy::has(DictionaryId tagIdToCheck) const
  {
    auto c = count();
    for (std::uint8_t i = 0; i < c; ++i)
    {
      if (id(i) == tagIdToCheck)
      {
        return true;
      }
    }
    return false;
  }

  // TrackView private implementation
  std::string_view TrackView::getString(std::uint16_t offset, std::uint16_t len) const
  {
    if (len == 0) { return {}; };

    std::size_t const start = sizeof(TrackHeader) + offset;
    if (start + len > _size)
    {
      return {};
    }

    return {utility::as<char>(_header, start), len};
  }

  std::uint32_t TrackView::tagId(std::uint8_t index) const
  {
    if (index >= _header->tagCount)
    {
      return 0;
    }
    auto offset = sizeof(TrackHeader) + _header->tagsOffset + index * sizeof(std::uint32_t);
    if (offset + sizeof(std::uint32_t) > _size)
    {
      return 0;
    }
    return *utility::as<std::uint32_t>(_header, offset);
  }

} // namespace rs::core
