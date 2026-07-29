// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListLayout.h>
#include <ao/library/ListView.h>
#include <ao/utility/ByteView.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <span>
#include <vector>

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

  ListBuilder ListBuilder::makeEmpty()
  {
    return ListBuilder{};
  }

  ListBuilder ListBuilder::fromView(ListView const& view)
  {
    auto builder = ListBuilder{};
    builder._parentId = view.parentId();
    builder._name = view.name();
    builder._description = view.description();
    builder._filter = view.filter();

    for (auto const id : view.orderTrackIds())
    {
      builder._orderTrackIdsBuilder.add(id);
    }

    return builder;
  }

  ListBuilder::OrderTrackIdsBuilder& ListBuilder::orderTrackIds()
  {
    return _orderTrackIdsBuilder;
  }

  ListBuilder& ListBuilder::name(std::string_view name)
  {
    _name = name;
    return *this;
  }

  ListBuilder& ListBuilder::description(std::string_view description)
  {
    _description = description;
    return *this;
  }

  ListBuilder& ListBuilder::filter(std::string_view filter)
  {
    _filter = filter;
    return *this;
  }

  ListBuilder& ListBuilder::parentId(ListId parentId)
  {
    _parentId = parentId;
    return *this;
  }

  ListBuilder::OrderTrackIdsBuilder& ListBuilder::OrderTrackIdsBuilder::add(TrackId id)
  {
    if (_trackIdMembership.insert(id).second)
    {
      _trackIds.push_back(id);
    }

    return *this;
  }

  ListBuilder::OrderTrackIdsBuilder& ListBuilder::OrderTrackIdsBuilder::remove(TrackId id)
  {
    std::erase(_trackIds, id);
    _trackIdMembership.erase(id);
    return *this;
  }

  ListBuilder::OrderTrackIdsBuilder& ListBuilder::OrderTrackIdsBuilder::clear()
  {
    _trackIds.clear();
    _trackIdMembership.clear();
    return *this;
  }

  Result<std::vector<std::byte>> ListBuilder::serialize() const
  {
    auto const& name = _name;
    auto const& description = _description;
    auto const& expression = _filter;
    auto const& orderTrackIds = _orderTrackIdsBuilder._trackIds;

    auto const nameLength = name.size();
    auto const descLength = description.size();
    auto const filterLength = expression.size();
    constexpr auto kMaxTextLength = static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max());
    constexpr auto kMaxStoredCount = static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());

    if (orderTrackIds.size() > kMaxStoredCount ||
        orderTrackIds.size() > std::numeric_limits<std::size_t>::max() / sizeof(TrackId))
    {
      return makeError(
        Error::Code::ValueTooLarge,
        std::format("List contains {} order track IDs; the count cannot be serialized", orderTrackIds.size()));
    }

    auto const orderTrackIdsSize = orderTrackIds.size() * sizeof(TrackId);

    if (nameLength > kMaxTextLength || descLength > kMaxTextLength || filterLength > kMaxTextLength)
    {
      return makeError(Error::Code::ValueTooLarge, "List text field exceeds the 65535-byte product limit");
    }

    std::size_t logicalSize = kListHeaderSize;

    if (!checkedAdd(logicalSize, orderTrackIdsSize) || !checkedAdd(logicalSize, nameLength) ||
        !checkedAdd(logicalSize, descLength) || !checkedAdd(logicalSize, filterLength) ||
        logicalSize > std::numeric_limits<std::size_t>::max() - (kListHeaderAlignment - 1))
    {
      return makeError(Error::Code::ValueTooLarge, "List record size overflows the host address space");
    }

    auto const payloadSize =
      (logicalSize + (kListHeaderAlignment - 1)) & ~(static_cast<std::size_t>(kListHeaderAlignment) - 1);

    auto result = std::vector<std::byte>{};
    result.reserve(payloadSize);

    auto header = ListHeader{};
    header.orderTrackIdCount = static_cast<std::uint32_t>(orderTrackIds.size());
    header.nameLength = static_cast<std::uint32_t>(nameLength);
    header.descLength = static_cast<std::uint32_t>(descLength);
    header.filterLength = static_cast<std::uint32_t>(filterLength);
    header.parentId = _parentId.raw();

    result.insert_range(result.end(), utility::bytes::view(header));

    if (!orderTrackIds.empty())
    {
      result.insert_range(result.end(), utility::bytes::view(std::span<TrackId const>{orderTrackIds}));
    }

    result.insert_range(result.end(), utility::bytes::view(name));
    result.insert_range(result.end(), utility::bytes::view(description));
    result.insert_range(result.end(), utility::bytes::view(expression));

    while (result.size() < payloadSize)
    {
      result.push_back(std::byte{0});
    }

    return result;
  }
} // namespace ao::library
