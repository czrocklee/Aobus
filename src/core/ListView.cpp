// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/core/ListView.h>
#include <rs/Exception.h>

namespace rs::core
{

  ListView::ListView(std::span<std::byte const> data)
    : _payload{data}
  {
    if (_payload.data() == nullptr || _payload.size() < kListHeaderSize)
    {
      RS_THROW(Exception, "Invalid data for ListView");
    }
  }

  std::string_view ListView::getString(std::uint16_t offset, std::uint16_t length) const
  {
    if (length == 0) { return {}; }

    auto const start = kListHeaderSize + offset;

    if (start + length > _payload.size()) { RS_THROW(Exception, "Invalid string field"); }

    return utility::bytes::stringView(_payload.subspan(start, length));
  }

  std::string_view ListView::name() const
  {
    return getString(header()->nameOffset, header()->nameLen);
  }

  std::string_view ListView::description() const
  {
    return getString(header()->descOffset, header()->descLen);
  }

  std::string_view ListView::filter() const
  {
    return getString(header()->filterOffset, header()->filterLen);
  }

  ListView::TrackProxy ListView::tracks() const
  {
    auto const offset = kListHeaderSize;
    auto const count = static_cast<std::size_t>(header()->trackIdsCount);

    if (offset + (count * sizeof(TrackId)) > _payload.size()) { RS_THROW(Exception, "Invalid trackIds field"); }

    return TrackProxy{utility::layout::viewArray<TrackId>(_payload.subspan(offset, count * sizeof(TrackId)))};
  }

  ListView::TrackProxy::TrackProxy(std::span<TrackId const> trackIds)
    : _trackIds{trackIds}
  {
  }

  TrackId ListView::TrackProxy::at(std::size_t index) const
  {
    if (index >= _trackIds.size()) { RS_THROW(Exception, "Index out of range"); }
    return _trackIds[index];
  }

} // namespace rs::core
