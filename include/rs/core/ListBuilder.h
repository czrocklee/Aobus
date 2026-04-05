// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/ListLayout.h>
#include <rs/core/ListRecord.h>
#include <rs/core/ListView.h>
#include <rs/core/Type.h>

#include <string>
#include <vector>

namespace rs::core
{

  /**
   * ListBuilder - Fluent builder for constructing list binary data.
   *
   * Separates concerns:
   *   - ListRecord: pure domain model (strings only)
   *   - ListBuilder: fluent API for population and serialization
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
    static ListBuilder fromRecord(ListRecord record);
    static ListBuilder fromView(ListView const& view);

    // Direct record access
    ListRecord& record() { return _record; }
    ListRecord const& record() const { return _record; }

    //=============================================================================
    // TracksBuilder - fluent TrackId add/remove (nested class)
    //=============================================================================
    class TracksBuilder;

    // Sub-builder accessor
    TracksBuilder tracks();

    // Direct setters
    ListBuilder& name(std::string v);
    ListBuilder& description(std::string v);
    ListBuilder& filter(std::string v);

    // Serialization - returns binary payload for ListStore
    std::vector<std::byte> serialize() const;

  private:
    explicit ListBuilder(ListRecord record);

    ListRecord _record{};
  };

  class ListBuilder::TracksBuilder
  {
  public:
    explicit TracksBuilder(ListBuilder& builder)
      : _builder{builder}
    {
    }

    TracksBuilder& add(TrackId id);
    TracksBuilder& remove(TrackId id);
    TracksBuilder& clear()
    {
      _builder._record.trackIds.clear();
      return *this;
    }

    std::vector<TrackId> const& ids() const { return _builder._record.trackIds; }

  private:
    ListBuilder& _builder;
  };

} // namespace rs::core
