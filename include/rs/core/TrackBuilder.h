// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/DictionaryStore.h>
#include <rs/core/TrackRecord.h>
#include <rs/core/TrackLayout.h>
#include <rs/core/TrackView.h>
#include <rs/lmdb/Transaction.h>

#include <functional>
#include <span>
#include <utility>
#include <vector>

namespace rs::core
{

class TrackBuilder;
class MetadataBuilder;
class PropertyBuilder;
class TagsBuilder;
class CustomBuilder;

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

  // Sub-builder accessors (lightweight proxies)
  MetadataBuilder metadata();
  PropertyBuilder property();
  TagsBuilder tags();
  CustomBuilder custom();

  // Full serialization - resolves all strings to DictionaryIds
  std::pair<std::vector<std::byte>, std::vector<std::byte>> serialize(
    DictionaryStore& dict, lmdb::WriteTransaction& txn) const;

  // Partial serialization for hot-only or cold-only updates
  std::vector<std::byte> serializeHot(DictionaryStore& dict, lmdb::WriteTransaction& txn) const;
  std::vector<std::byte> serializeCold(DictionaryStore& dict, lmdb::WriteTransaction& txn) const;

private:
  explicit TrackBuilder(TrackRecord record);

  TrackRecord _record{};

  // Private helper methods for serialization
  static std::uint32_t computeBloomFilter(std::span<DictionaryId const> tagIds);
  TrackHotHeader buildHotHeader(DictionaryStore& dict, lmdb::WriteTransaction& txn, std::span<DictionaryId const> tagIds) const;
  TrackColdHeader buildColdHeader(std::size_t customCount, std::uint16_t uriOffset, std::uint16_t uriLen) const;
};

//=============================================================================
// MetadataBuilder - fluent string setters
//=============================================================================
class MetadataBuilder
{
public:
  explicit MetadataBuilder(TrackRecord& record) : _record{record} {}

  MetadataBuilder& title(std::string v) { _record.metadata.title = std::move(v); return *this; }
  MetadataBuilder& uri(std::string v) { _record.metadata.uri = std::move(v); return *this; }
  MetadataBuilder& artist(std::string v) { _record.metadata.artist = std::move(v); return *this; }
  MetadataBuilder& album(std::string v) { _record.metadata.album = std::move(v); return *this; }
  MetadataBuilder& albumArtist(std::string v) { _record.metadata.albumArtist = std::move(v); return *this; }
  MetadataBuilder& genre(std::string v) { _record.metadata.genre = std::move(v); return *this; }
  MetadataBuilder& year(std::uint16_t v) { _record.metadata.year = v; return *this; }
  MetadataBuilder& trackNumber(std::uint16_t v) { _record.metadata.trackNumber = v; return *this; }
  MetadataBuilder& totalTracks(std::uint16_t v) { _record.metadata.totalTracks = v; return *this; }
  MetadataBuilder& discNumber(std::uint16_t v) { _record.metadata.discNumber = v; return *this; }
  MetadataBuilder& totalDiscs(std::uint16_t v) { _record.metadata.totalDiscs = v; return *this; }
  MetadataBuilder& coverArtId(std::uint32_t v) { _record.metadata.coverArtId = v; return *this; }

private:
  TrackRecord& _record;
};

//=============================================================================
// PropertyBuilder - fluent numeric setters
//=============================================================================
class PropertyBuilder
{
public:
  explicit PropertyBuilder(TrackRecord& record) : _record{record} {}

  PropertyBuilder& fileSize(std::uint64_t v) { _record.property.fileSize = v; return *this; }
  PropertyBuilder& mtime(std::uint64_t v) { _record.property.mtime = v; return *this; }
  PropertyBuilder& durationMs(std::uint32_t v) { _record.property.durationMs = v; return *this; }
  PropertyBuilder& bitrate(std::uint32_t v) { _record.property.bitrate = v; return *this; }
  PropertyBuilder& sampleRate(std::uint32_t v) { _record.property.sampleRate = v; return *this; }
  PropertyBuilder& codecId(std::uint16_t v) { _record.property.codecId = v; return *this; }
  PropertyBuilder& channels(std::uint8_t v) { _record.property.channels = v; return *this; }
  PropertyBuilder& bitDepth(std::uint8_t v) { _record.property.bitDepth = v; return *this; }
  PropertyBuilder& rating(std::uint8_t v) { _record.property.rating = v; return *this; }

private:
  TrackRecord& _record;
};

//=============================================================================
// TagsBuilder - fluent tag name add/remove
//=============================================================================
class TagsBuilder
{
public:
  explicit TagsBuilder(TrackRecord& record) : _record{record} {}

  TagsBuilder& add(std::string tagName);
  TagsBuilder& remove(std::string tagName);
  TagsBuilder& clear() { _record.tags.names.clear(); return *this; }

  std::vector<std::string> const& names() const { return _record.tags.names; }

private:
  TrackRecord& _record;
};

//=============================================================================
// CustomBuilder - fluent key-value add/remove
//=============================================================================
class CustomBuilder
{
public:
  explicit CustomBuilder(TrackRecord& record) : _record{record} {}

  CustomBuilder& add(std::string key, std::string value);
  CustomBuilder& remove(std::string key);
  CustomBuilder& clear() { _record.custom.pairs.clear(); return *this; }

  std::vector<std::pair<std::string, std::string>> const& pairs() const { return _record.custom.pairs; }

private:
  TrackRecord& _record;
};

} // namespace rs::core
