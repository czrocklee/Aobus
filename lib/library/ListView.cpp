// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/library/ListView.h>

#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/library/ListLayout.h>
#include <ao/utility/ByteView.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

namespace ao::library
{
  namespace
  {
    bool checkedAdd(std::size_t& value, std::size_t const amount) noexcept
    {
      if (amount > std::numeric_limits<std::size_t>::max() - value)
      {
        return false;
      }

      value += amount;
      return true;
    }
  } // namespace

  TrackId ListView::OrderTrackIdProxy::at(std::size_t index) const noexcept
  {
    AO_EXPECTS(index < _trackIds.size());
    return _trackIds[index];
  }

  ListView::ListView(std::span<std::byte const> data) noexcept
    : _payload{data}
  {
    auto const* header = utility::bytes::tryLayout<ListHeader>(data);

    if (header == nullptr)
    {
      return;
    }

    auto const orderTrackIdBytes64 =
      std::uint64_t{header->orderTrackIdCount} * static_cast<std::uint64_t>(sizeof(TrackId));

    if (!std::in_range<std::size_t>(orderTrackIdBytes64))
    {
      return;
    }

    auto const orderTrackIdBytes = static_cast<std::size_t>(orderTrackIdBytes64);
    std::size_t variableSize = orderTrackIdBytes;
    auto const nameOffset = variableSize;

    if (!checkedAdd(variableSize, header->nameLength))
    {
      return;
    }

    auto const descOffset = variableSize;

    if (!checkedAdd(variableSize, header->descLength))
    {
      return;
    }

    auto const filterOffset = variableSize;

    if (!checkedAdd(variableSize, header->filterLength))
    {
      return;
    }

    std::size_t logicalSize = kListHeaderSize;

    if (!checkedAdd(logicalSize, variableSize) ||
        logicalSize > std::numeric_limits<std::size_t>::max() - (kListHeaderAlignment - 1))
    {
      return;
    }

    auto const recordSize =
      (logicalSize + (kListHeaderAlignment - 1)) & ~(static_cast<std::size_t>(kListHeaderAlignment) - 1);

    if (data.size() != recordSize)
    {
      return;
    }

    for (auto const byte : data.subspan(logicalSize))
    {
      if (byte != std::byte{0})
      {
        return;
      }
    }

    _nameOffset = nameOffset;
    _descOffset = descOffset;
    _filterOffset = filterOffset;
    _header = header;
  }

  std::string_view ListView::stringAt(std::size_t offset, std::uint32_t length) const noexcept
  {
    return utility::bytes::stringView(_payload.subspan(kListHeaderSize + offset, length));
  }

  std::string_view ListView::name() const noexcept
  {
    return _header == nullptr ? std::string_view{} : stringAt(_nameOffset, _header->nameLength);
  }

  std::string_view ListView::description() const noexcept
  {
    return _header == nullptr ? std::string_view{} : stringAt(_descOffset, _header->descLength);
  }

  std::string_view ListView::filter() const noexcept
  {
    return _header == nullptr ? std::string_view{} : stringAt(_filterOffset, _header->filterLength);
  }

  ListId ListView::parentId() const noexcept
  {
    return _header == nullptr ? kInvalidListId : ListId{_header->parentId};
  }

  bool ListView::isRootParent() const noexcept
  {
    return parentId() == kInvalidListId;
  }

  ListView::OrderTrackIdProxy ListView::orderTrackIds() const noexcept
  {
    if (_header == nullptr)
    {
      return OrderTrackIdProxy{};
    }

    auto const orderTrackIdBytes = static_cast<std::size_t>(_header->orderTrackIdCount) * sizeof(TrackId);
    return OrderTrackIdProxy{utility::layout::viewArray<TrackId>(_payload.subspan(kListHeaderSize, orderTrackIdBytes))};
  }
} // namespace ao::library
