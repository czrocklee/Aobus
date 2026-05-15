// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/library/ListView.h>
#include <ao/library/ListLayout.h>
#include <ao/Exception.h>
#include <ao/Type.h>
#include <ao/utility/ByteView.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace ao::library
{
  ListView::ListView(std::span<std::byte const> data)
    : _payload{data}
  {
    if (_payload.data() == nullptr || _payload.size() < kListHeaderSize)
    {
      ao::throwException<Exception>("Invalid data for ListView");
    }
  }

  std::string_view ListView::getString(std::uint16_t offset, std::uint16_t length) const
  {
    if (length == 0)
    {
      return {};
    }

    auto const start = kListHeaderSize + offset;

    if (start + length > _payload.size())
    {
      ao::throwException<Exception>("Invalid string field");
    }

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

  ListId ListView::parentId() const noexcept
  {
    return ListId{header()->parentId};
  }

  bool ListView::isRootParent() const noexcept
  {
    return parentId() == ListId{0};
  }

  ListView::TrackProxy ListView::tracks() const
  {
    auto const offset = kListHeaderSize;
    auto const count = static_cast<std::size_t>(header()->trackIdsCount);

    if (offset + (count * sizeof(TrackId)) > _payload.size())
    {
      ao::throwException<Exception>("Invalid trackIds field");
    }

    return TrackProxy{utility::layout::viewArray<TrackId>(_payload.subspan(offset, count * sizeof(TrackId)))};
  }

  ListView::TrackProxy::TrackProxy(std::span<TrackId const> trackIds)
    : _trackIds{trackIds}
  {
  }

  TrackId ListView::TrackProxy::at(std::size_t index) const
  {
    if (index >= _trackIds.size())
    {
      ao::throwException<Exception>("Index out of range");
    }

    return _trackIds[index];
  }
} // namespace ao::library
