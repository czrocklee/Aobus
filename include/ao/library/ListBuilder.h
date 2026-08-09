// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/ListView.h>

#include <cstddef>
#include <span>
#include <string>
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

    /** Immutable, canonically validated bytes for one List Store write. */
    class Prepared final
    {
    public:
      std::size_t size() const noexcept { return _bytes.size(); }
      std::span<std::byte const> bytes() const noexcept { return _bytes; }
      void writeTo(std::span<std::byte> out) const noexcept;

    private:
      explicit Prepared(std::vector<std::byte> bytes);

      std::vector<std::byte> _bytes;

      friend class ListBuilder;
    };

    // Preparation validates every field, every derived extent, and the final
    // canonical record.
    Result<Prepared> prepare() const;

    // Standalone serialization follows the same canonical preparation path.
    // Corruption diagnostics mutate the returned bytes explicitly; production
    // Store writers accept Prepared only.
    Result<std::vector<std::byte>> serialize() const;

  private:
    explicit ListBuilder() = default;

    Result<std::vector<std::byte>> serializeCandidate() const;

    std::string _name;
    std::string _description;
    std::string _filter;
    ListId _parentId = kInvalidListId;

    OrderTrackIdsBuilder _orderTrackIdsBuilder;
  };
} // namespace ao::library
