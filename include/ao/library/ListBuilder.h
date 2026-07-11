// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/library/ListView.h>

#include <cstddef>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace ao::library
{
  /**
   * ListBuilder - Fluent builder for constructing list binary data.
   *
   * Stores strings as string_view pointing to external data.
   *
   * Usage:
   *   // Create a manual list
   *   auto builder = ListBuilder::makeEmpty();
   *   builder.name("My Playlist").description("Great songs");
   *   builder.tracks().add(trackId1).add(trackId2);
   *   auto payload = builder.serialize();
   *
   *   // Create a smart list
   *   auto builder = ListBuilder::makeEmpty();
   *   builder.name("Smart List").description("Filtered").filter("@genre = rock");
   *   auto payload = builder.serialize();
   */
  class ListBuilder
  {
  public:
    // Factory methods
    static ListBuilder makeEmpty();
    static ListBuilder fromView(ListView const& view);

    //=============================================================================
    // TracksBuilder - nested class for track management
    //=============================================================================
    class TracksBuilder
    {
    public:
      explicit TracksBuilder() = default;

      /** Adds an ID only on its first occurrence, preserving request order. */
      TracksBuilder& add(TrackId id);

      /** Removes every occurrence, including duplicates from legacy records. */
      TracksBuilder& remove(TrackId id);
      TracksBuilder& clear();
      TracksBuilder& smart(bool smart);

      std::vector<TrackId> const& ids() const { return _trackIds; }
      bool isSmart() const { return _isSmart; }

    private:
      friend class ListBuilder;

      std::vector<TrackId> _trackIds;
      std::unordered_set<TrackId> _trackIdMembership;
      bool _isSmart = false;
    };

    // Sub-builder accessor
    TracksBuilder& tracks();

    // Direct setters
    ListBuilder& name(std::string_view name);
    ListBuilder& description(std::string_view description);
    ListBuilder& filter(std::string_view filter);
    ListBuilder& parentId(ListId parentId);

    // Serialization - returns binary payload for ListStore
    std::vector<std::byte> serialize() const;

  private:
    explicit ListBuilder() = default;

    // Data storage - string_view pointing to external data
    std::string_view _name;
    std::string_view _description;
    std::string_view _filter;
    ListId _parentId = kInvalidListId;

    // TracksBuilder needs access to modify ListBuilder's isSmart flag
    friend class TracksBuilder;

    // Sub-builder stored as member
    TracksBuilder _tracksBuilder;
  };
} // namespace ao::library
