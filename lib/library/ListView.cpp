// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/library/ListLayout.h>
#include <ao/library/ListView.h>
#include <ao/utility/ByteView.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace ao::library
{
  ListView::ListView(std::span<std::byte const> data) noexcept
    : _payload{data}
  {
    auto const* header = utility::bytes::tryLayout<ListHeader>(data);

    if (header == nullptr)
    {
      return;
    }

    auto const size = data.size();
    auto const trackIdBytes = static_cast<std::size_t>(header->trackIdsCount) * sizeof(TrackId);

    auto fieldFits = [size](std::uint16_t offset, std::uint16_t length) noexcept
    { return kListHeaderSize + static_cast<std::size_t>(offset) + static_cast<std::size_t>(length) <= size; };

    if (trackIdBytes > size - kListHeaderSize || !fieldFits(header->nameOffset, header->nameLength) ||
        !fieldFits(header->descOffset, header->descLength) || !fieldFits(header->filterOffset, header->filterLength))
    {
      return;
    }

    _header = header;
  }

  std::string_view ListView::stringAt(std::uint16_t offset, std::uint16_t length) const noexcept
  {
    return utility::bytes::stringView(_payload.subspan(kListHeaderSize + offset, length));
  }

  std::string_view ListView::name() const noexcept
  {
    return _header == nullptr ? std::string_view{} : stringAt(_header->nameOffset, _header->nameLength);
  }

  std::string_view ListView::description() const noexcept
  {
    return _header == nullptr ? std::string_view{} : stringAt(_header->descOffset, _header->descLength);
  }

  std::string_view ListView::filter() const noexcept
  {
    return _header == nullptr ? std::string_view{} : stringAt(_header->filterOffset, _header->filterLength);
  }

  ListId ListView::parentId() const noexcept
  {
    return _header == nullptr ? kInvalidListId : ListId{_header->parentId};
  }

  bool ListView::isRootParent() const noexcept
  {
    return parentId() == kInvalidListId;
  }

  ListView::TrackProxy ListView::tracks() const noexcept
  {
    if (_header == nullptr)
    {
      return TrackProxy{};
    }

    auto const trackIdBytes = static_cast<std::size_t>(_header->trackIdsCount) * sizeof(TrackId);
    return TrackProxy{utility::layout::viewArray<TrackId>(_payload.subspan(kListHeaderSize, trackIdBytes))};
  }
} // namespace ao::library
