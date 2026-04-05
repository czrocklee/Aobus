// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/DictionaryStore.h>
#include <rs/core/TrackLayout.h>
#include <rs/core/TrackRecord.h>
#include <rs/core/TrackView.h>
#include <rs/lmdb/Transaction.h>

#include <span>
#include <utility>
#include <vector>

namespace rs::core
{

  /**
   * TrackBuilder - Fluent builder for constructing track binary data.
   *
   * Separates concerns:
   *   - TrackRecord: pure domain model (strings only, no DictionaryIds)
   *   - TrackBuilder: fluent API for population, resolves strings to DictionaryIds at serialize()
   *
   * Usage:
   *   // Pattern A: from existing record, modify hot only
   *   auto builder = TrackBuilder::fromRecord(existingRecord);
   *   builder.tags().add("rock").remove("jazz");
   *   auto hotData = builder.serializeHot(dict, txn);
   *   writer.updateHot(trackId, hotData);
   *
   *   // Pattern B: create new track
   *   auto builder = TrackBuilder::createNew();
   *   builder.metadata().title("Song").artist("Artist").album("Album");
   *   builder.property().fileSize(fs).bitDepth(16);
   *   builder.tags().add("rock");
   *   builder.custom().add("key", "value");
   *   auto [hot, cold] = builder.serialize(dict, txn);
   *   writer.createHotCold(hot, cold);
   */
  class TrackBuilder
  {
  public:
    // Factory methods
    static TrackBuilder createNew();
    static TrackBuilder fromRecord(TrackRecord record);
    static TrackBuilder fromView(TrackView const& view, DictionaryStore& dict);

    // Direct record access
    TrackRecord& record() { return _record; }
    TrackRecord const& record() const { return _record; }

    //=============================================================================
    // Sub-builders - fluent setters (nested class declarations)
    //=============================================================================
    class MetadataBuilder;
    class PropertyBuilder;
    class TagsBuilder;
    class CustomBuilder;

    // Sub-builder accessors
    MetadataBuilder metadata();
    PropertyBuilder property();
    TagsBuilder tags();
    CustomBuilder custom();

    // Full serialization - resolves all strings to DictionaryIds
    std::pair<std::vector<std::byte>, std::vector<std::byte>> serialize(DictionaryStore& dict,
                                                                        lmdb::WriteTransaction& txn) const;

    // Partial serialization for hot-only or cold-only updates
    std::vector<std::byte> serializeHot(DictionaryStore& dict, lmdb::WriteTransaction& txn) const;
    std::vector<std::byte> serializeCold(DictionaryStore& dict, lmdb::WriteTransaction& txn) const;

  private:
    explicit TrackBuilder(TrackRecord record);

    TrackRecord _record{};

    // Private helper methods for serialization
    static std::uint32_t computeBloomFilter(std::span<DictionaryId const> tagIds);
    TrackHotHeader buildHotHeader(DictionaryStore& dict,
                                  lmdb::WriteTransaction& txn,
                                  std::span<DictionaryId const> tagIds) const;
    TrackColdHeader buildColdHeader(std::size_t customCount, std::uint16_t uriOffset, std::uint16_t uriLen) const;
  };

  //=============================================================================
  // MetadataBuilder - fluent string setters
  //=============================================================================
  class TrackBuilder::MetadataBuilder
  {
  public:
    explicit MetadataBuilder(TrackBuilder& builder)
      : _builder{builder}
    {
    }

    MetadataBuilder& title(std::string v);
    MetadataBuilder& uri(std::string v);
    MetadataBuilder& artist(std::string v);
    MetadataBuilder& album(std::string v);
    MetadataBuilder& albumArtist(std::string v);
    MetadataBuilder& genre(std::string v);
    MetadataBuilder& year(std::uint16_t v);
    MetadataBuilder& trackNumber(std::uint16_t v);
    MetadataBuilder& totalTracks(std::uint16_t v);
    MetadataBuilder& discNumber(std::uint16_t v);
    MetadataBuilder& totalDiscs(std::uint16_t v);
    MetadataBuilder& coverArtId(std::uint32_t v);

  private:
    TrackBuilder& _builder;
  };

  //=============================================================================
  // PropertyBuilder - fluent numeric setters
  //=============================================================================
  class TrackBuilder::PropertyBuilder
  {
  public:
    explicit PropertyBuilder(TrackBuilder& builder)
      : _builder{builder}
    {
    }

    PropertyBuilder& fileSize(std::uint64_t v);
    PropertyBuilder& mtime(std::uint64_t v);
    PropertyBuilder& durationMs(std::uint32_t v);
    PropertyBuilder& bitrate(std::uint32_t v);
    PropertyBuilder& sampleRate(std::uint32_t v);
    PropertyBuilder& codecId(std::uint16_t v);
    PropertyBuilder& channels(std::uint8_t v);
    PropertyBuilder& bitDepth(std::uint8_t v);
    PropertyBuilder& rating(std::uint8_t v);

  private:
    TrackBuilder& _builder;
  };

  //=============================================================================
  // TagsBuilder - fluent tag name add/remove
  //=============================================================================
  class TrackBuilder::TagsBuilder
  {
  public:
    explicit TagsBuilder(TrackBuilder& builder)
      : _builder{builder}
    {
    }

    TagsBuilder& add(std::string tagName);
    TagsBuilder& remove(std::string tagName);
    TagsBuilder& clear();

    std::vector<std::string> const& names() const;

  private:
    TrackBuilder& _builder;
  };

  //=============================================================================
  // CustomBuilder - fluent key-value add/remove
  //=============================================================================
  class TrackBuilder::CustomBuilder
  {
  public:
    explicit CustomBuilder(TrackBuilder& builder)
      : _builder{builder}
    {
    }

    CustomBuilder& add(std::string key, std::string value);
    CustomBuilder& remove(std::string key);
    CustomBuilder& clear();

    std::vector<std::pair<std::string, std::string>> const& pairs() const;

  private:
    TrackBuilder& _builder;
  };

} // namespace rs::core
