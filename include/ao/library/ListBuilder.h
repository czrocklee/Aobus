// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/ListView.h>

#include <cstddef>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace ao::library
{
  /** Fluent builder for one saved List record. */
  class ListBuilder
  {
  public:
    static ListBuilder makeEmpty();
    static ListBuilder fromView(ListView const& view);

    class OrderTrackIdsBuilder
    {
    public:
      explicit OrderTrackIdsBuilder() = default;

      /** Adds an ID only on its first occurrence, preserving request order. */
      OrderTrackIdsBuilder& add(TrackId id);

      /** Removes every occurrence. */
      OrderTrackIdsBuilder& remove(TrackId id);
      OrderTrackIdsBuilder& clear();

      std::vector<TrackId> const& ids() const { return _trackIds; }

    private:
      friend class ListBuilder;

      std::vector<TrackId> _trackIds;
      std::unordered_set<TrackId> _trackIdMembership;
    };

    OrderTrackIdsBuilder& orderTrackIds();

    // Direct setters
    ListBuilder& name(std::string_view name);
    ListBuilder& description(std::string_view description);
    ListBuilder& filter(std::string_view filter);
    ListBuilder& parentId(ListId parentId);

    // Serialization validates every field and every derived extent.
    Result<std::vector<std::byte>> serialize() const;

  private:
    explicit ListBuilder() = default;

    std::string_view _name;
    std::string_view _description;
    std::string_view _filter;
    ListId _parentId = kInvalidListId;

    OrderTrackIdsBuilder _orderTrackIdsBuilder;
  };
} // namespace ao::library
