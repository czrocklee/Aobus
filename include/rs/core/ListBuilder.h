// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/ListLayout.h>
#include <rs/core/ListRecord.h>
#include <rs/core/ListView.h>
#include <rs/core/Type.h>

#include <string_view>
#include <vector>

namespace rs::core
{

  /**
   * ListBuilder - Fluent builder for constructing list binary data.
   *
   * Stores strings as string_view pointing to external data.
   *
   * Usage:
   *   // Create a manual list
   *   auto builder = ListBuilder::createNew();
   *   builder.name("My Playlist").description("Great songs");
   *   builder.tracks().add(trackId1).add(trackId2);
   *   auto payload = builder.serialize();
   *
   *   // Create a smart list
   *   auto builder = ListBuilder::createNew();
   *   builder.name("Smart List").description("Filtered").filter("@genre = rock");
   *   auto payload = builder.serialize();
   */
  class ListBuilder
  {
  public:
    // Factory methods
    static ListBuilder createNew();
    static ListBuilder fromRecord(ListRecord const& record);
    static ListBuilder fromView(ListView const& view);

    // Record access - constructs ListRecord on-the-fly
    ListRecord record() const;

    //=============================================================================
    // TracksBuilder - nested class for track management
    //=============================================================================
    class TracksBuilder
    {
    public:
      explicit TracksBuilder(ListBuilder& builder)
        : _builder{builder}
      {
      }

      TracksBuilder& add(TrackId id);
      TracksBuilder& remove(TrackId id);
      TracksBuilder& clear();
      TracksBuilder& isSmart(bool v);

      std::vector<TrackId> const& ids() const { return _trackIds; }
      bool isSmart() const { return _isSmart; }

    private:
      friend class ListBuilder;

      ListBuilder& _builder;
      std::vector<TrackId> _trackIds;
      bool _isSmart = false;
    };

    // Sub-builder accessor
    TracksBuilder& tracks();

    // Direct setters
    ListBuilder& name(std::string_view v);
    ListBuilder& description(std::string_view v);
    ListBuilder& filter(std::string_view v);
    ListBuilder& sourceListId(ListId v);

    // Serialization - returns binary payload for ListStore
    std::vector<std::byte> serialize() const;

  private:
    explicit ListBuilder() = default;

    // Data storage - string_view pointing to external data
    std::string_view _name;
    std::string_view _description;
    std::string_view _filter;
    ListId _sourceListId = ListId{0};

    // TracksBuilder needs access to modify ListBuilder's isSmart flag
    friend class TracksBuilder;

    // Sub-builder stored as member
    TracksBuilder _tracksBuilder{*this};
  };

} // namespace rs::core
