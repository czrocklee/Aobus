// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/DictionaryStore.h>
#include <rs/core/ResourceStore.h>
#include <rs/core/TrackLayout.h>
#include <rs/core/TrackRecord.h>
#include <rs/core/TrackView.h>
#include <rs/lmdb/Transaction.h>

#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace rs::core
{

  /**
   * TrackBuilder - Fluent builder for constructing track binary data.
   *
   * Stores strings as string_view pointing to external data (from Record or mmap).
   * Sub-builders hold the actual data as member variables.
   *
   * Usage:
   *   // Pattern A: from existing record, modify hot only
   *   auto builder = TrackBuilder::fromRecord(existingRecord);
   *   builder.tags().add("rock").remove("jazz");
   *   auto hotData = builder.serializeHot(txn, dict);
   *   writer.updateHot(trackId, hotData);
   *
   *   // Pattern B: create new track
   *   auto builder = TrackBuilder::createNew();
   *   builder.metadata().title("Song").artist("Artist").album("Album");
   *   builder.property().fileSize(fs).bitDepth(16);
   *   builder.tags().add("rock");
   *   builder.custom().add("key", "value");
   *   auto [hot, cold] = builder.serialize(txn, dict, resources);
   *   writer.createHotCold(hot, cold);
   */
  class TrackBuilder
  {
  public:
    // Factory methods
    static TrackBuilder createNew();
    static TrackBuilder fromRecord(TrackRecord const& record);
    static TrackBuilder fromView(TrackView const& view, DictionaryStore& dict);

    // Record access - constructs TrackRecord on-the-fly from stored data
    TrackRecord record() const;

    //=============================================================================
    // Sub-builders - own the data as string_view
    //=============================================================================
    class MetadataBuilder
    {
    public:
      // String setters
      MetadataBuilder& title(std::string_view v);
      MetadataBuilder& artist(std::string_view v);
      MetadataBuilder& album(std::string_view v);
      MetadataBuilder& albumArtist(std::string_view v);
      MetadataBuilder& genre(std::string_view v);

      // Numeric setters
      MetadataBuilder& year(std::uint16_t v);
      MetadataBuilder& trackNumber(std::uint16_t v);
      MetadataBuilder& totalTracks(std::uint16_t v);
      MetadataBuilder& discNumber(std::uint16_t v);
      MetadataBuilder& totalDiscs(std::uint16_t v);
      MetadataBuilder& coverArtId(std::uint32_t v);
      MetadataBuilder& coverArtData(std::span<std::byte const> v);
      MetadataBuilder& rating(std::uint8_t v);

      // Accessors
      std::string_view title() const { return _title; }
      std::string_view artist() const { return _artist; }
      std::string_view album() const { return _album; }
      std::string_view albumArtist() const { return _albumArtist; }
      std::string_view genre() const { return _genre; }

    private:
      friend class TrackBuilder;

      // Metadata strings - string_view pointing to external data
      std::string_view _title;
      std::string_view _artist;
      std::string_view _album;
      std::string_view _albumArtist;
      std::string_view _genre;

      // Metadata numerics
      std::uint16_t _year = 0;
      std::uint16_t _trackNumber = 0;
      std::uint16_t _totalTracks = 0;
      std::uint16_t _discNumber = 0;
      std::uint16_t _totalDiscs = 0;
      std::uint32_t _coverArtId = 0;
      std::uint8_t _rating = 0;
      mutable std::span<std::byte const> _embeddedCoverArt;
    };

    class PropertyBuilder
    {
    public:
      PropertyBuilder& fileSize(std::uint64_t v);
      PropertyBuilder& mtime(std::uint64_t v);
      PropertyBuilder& durationMs(std::uint32_t v);
      PropertyBuilder& bitrate(std::uint32_t v);
      PropertyBuilder& sampleRate(std::uint32_t v);
      PropertyBuilder& codecId(std::uint16_t v);
      PropertyBuilder& channels(std::uint8_t v);
      PropertyBuilder& bitDepth(std::uint8_t v);
      PropertyBuilder& uri(std::string_view v);

      // Accessors
      std::string_view uri() const { return _uri; }

    private:
      friend class TrackBuilder;

      // Property numerics
      std::uint64_t _fileSize = 0;
      std::uint64_t _mtime = 0;
      std::uint32_t _durationMs = 0;
      std::uint32_t _bitrate = 0;
      std::uint32_t _sampleRate = 0;
      std::uint16_t _codecId = 0;
      std::uint8_t _channels = 0;
      std::uint8_t _bitDepth = 0;

      // Property strings
      std::string_view _uri;
    };

    class TagsBuilder
    {
    public:
      TagsBuilder& add(std::string_view tagName);
      TagsBuilder& remove(std::string_view tagName);
      TagsBuilder& clear();

      std::vector<std::string_view> const& names() const { return _tagNames; }

    private:
      friend class TrackBuilder;

      // Tag names - string_view pointing to external data
      std::vector<std::string_view> _tagNames;
    };

    class CustomBuilder
    {
    public:
      CustomBuilder& add(std::string_view key, std::string_view value);
      CustomBuilder& remove(std::string_view key);
      CustomBuilder& clear();

      std::vector<std::pair<std::string_view, std::string_view>> const& pairs() const { return _customPairs; }

    private:
      friend class TrackBuilder;

      // Custom pairs - string_view pointing to external data
      std::vector<std::pair<std::string_view, std::string_view>> _customPairs;
    };

    // Sub-builder accessors - return references to stored members
    MetadataBuilder& metadata();
    PropertyBuilder& property();
    TagsBuilder& tags();
    CustomBuilder& custom();

    // Full serialization - resolves all strings to DictionaryIds
    std::pair<std::vector<std::byte>, std::vector<std::byte>> serialize(lmdb::WriteTransaction& txn,
                                                                        DictionaryStore& dict,
                                                                        ResourceStore& resources);

    // Partial serialization for hot-only or cold-only updates
    std::vector<std::byte> serializeHot(lmdb::WriteTransaction& txn,
                                       DictionaryStore& dict);
    std::vector<std::byte> serializeCold(lmdb::WriteTransaction& txn,
                                         DictionaryStore& dict,
                                         ResourceStore& resources);

  private:
    // Private helper methods for serialization
    static std::uint32_t computeBloomFilter(std::span<DictionaryId const> tagIds);
    TrackHotHeader buildHotHeader(DictionaryStore& dict,
                                  lmdb::WriteTransaction& txn,
                                  std::span<DictionaryId const> tagIds) const;
    TrackColdHeader buildColdHeader(std::size_t customCount, std::uint16_t uriOffset, std::uint16_t uriLen) const;
    void commitEmbeddedCoverArt(lmdb::WriteTransaction& txn, ResourceStore& resources);

    // Sub-builders stored as members
    MetadataBuilder _metadataBuilder{};
    PropertyBuilder _propertyBuilder{};
    TagsBuilder _tagsBuilder{};
    CustomBuilder _customBuilder{};
  };

} // namespace rs::core