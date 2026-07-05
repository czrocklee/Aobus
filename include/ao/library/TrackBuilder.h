// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/CoverArt.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackView.h>
#include <ao/lmdb/Transaction.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ao::library
{
  /**
   * TrackBuilder - Fluent builder for constructing track binary data.
   *
   * Stores strings as string_view pointing to external data (from std::string or mmap).
   * Sub-builders hold the actual data as member variables.
   *
   * Usage:
   *   // Pattern A: from existing view, modify hot only
   *   auto builder = TrackBuilder::fromView(view, dict);
   *   builder.tags().add("rock").remove("jazz");
   *   auto hotData = builder.serializeHot(txn, dict);
   *   writer.updateHot(trackId, hotData);
   *
   *   // Pattern B: create new track
   *   auto builder = TrackBuilder::createNew();
   *   builder.metadata().title("Song").artist("Artist").album("Album");
   *   builder.property().fileSize(fs).bitDepth(BitDepth{16});
   *   builder.tags().add("rock");
   *   builder.customMetadata().add("key", "value");
   *   auto [hot, cold] = builder.serialize(txn, dict, resources);
   *   writer.createHotCold(hot, cold);
   */
  class TrackBuilder final
  {
  public:
    // Factory methods
    static TrackBuilder createNew();
    static TrackBuilder fromView(TrackView const& view, DictionaryStore& dict);

    //=============================================================================
    // Sub-builders - own the data as string_view
    //=============================================================================
    class MetadataBuilder
    {
    public:
      // String setters
      MetadataBuilder& title(std::string_view text);
      MetadataBuilder& artist(std::string_view text);
      MetadataBuilder& album(std::string_view text);
      MetadataBuilder& albumArtist(std::string_view text);
      MetadataBuilder& composer(std::string_view text);
      MetadataBuilder& conductor(std::string_view text);
      MetadataBuilder& ensemble(std::string_view text);
      MetadataBuilder& genre(std::string_view text);
      MetadataBuilder& work(std::string_view text);
      MetadataBuilder& movement(std::string_view text);
      MetadataBuilder& soloist(std::string_view text);

      // Numeric setters
      MetadataBuilder& year(std::uint16_t year);
      MetadataBuilder& trackNumber(std::uint16_t number);
      MetadataBuilder& trackTotal(std::uint16_t count);
      MetadataBuilder& discNumber(std::uint16_t number);
      MetadataBuilder& discTotal(std::uint16_t count);
      MetadataBuilder& movementNumber(std::uint16_t number);
      MetadataBuilder& movementTotal(std::uint16_t count);

      // Accessors
      std::string_view title() const { return _title; }
      std::string_view artist() const { return _artist; }
      std::string_view album() const { return _album; }
      std::string_view albumArtist() const { return _albumArtist; }
      std::string_view composer() const { return _composer; }
      std::string_view conductor() const { return _conductor; }
      std::string_view ensemble() const { return _ensemble; }
      std::string_view genre() const { return _genre; }
      std::string_view work() const { return _work; }
      std::string_view movement() const { return _movement; }
      std::string_view soloist() const { return _soloist; }
      std::uint16_t year() const { return _year; }
      std::uint16_t trackNumber() const { return _trackNumber; }
      std::uint16_t trackTotal() const { return _trackTotal; }
      std::uint16_t discNumber() const { return _discNumber; }
      std::uint16_t discTotal() const { return _discTotal; }
      std::uint16_t movementNumber() const { return _movementNumber; }
      std::uint16_t movementTotal() const { return _movementTotal; }

    private:
      friend class TrackBuilder;

      // Metadata strings - string_view pointing to external data
      std::string_view _title;
      std::string_view _artist;
      std::string_view _album;
      std::string_view _albumArtist;
      std::string_view _composer;
      std::string_view _conductor;
      std::string_view _ensemble;
      std::string_view _genre;
      std::string_view _work;
      std::string_view _movement;
      std::string_view _soloist;

      // Metadata numerics
      std::uint16_t _year = 0;
      std::uint16_t _trackNumber = 0;
      std::uint16_t _trackTotal = 0;
      std::uint16_t _discNumber = 0;
      std::uint16_t _discTotal = 0;
      std::uint16_t _movementNumber = 0;
      std::uint16_t _movementTotal = 0;
    };

    class PropertyBuilder
    {
    public:
      PropertyBuilder& duration(std::chrono::milliseconds duration);
      PropertyBuilder& bitrate(Bitrate bitrate);
      PropertyBuilder& sampleRate(SampleRate sampleRate);
      PropertyBuilder& codec(AudioCodec codec);
      PropertyBuilder& channels(Channels channels);
      PropertyBuilder& bitDepth(BitDepth bitDepth);
      PropertyBuilder& uri(std::string_view uri);

      // Accessors
      std::string_view uri() const { return _uri; }
      std::chrono::milliseconds duration() const { return _duration; }
      Bitrate bitrate() const { return _bitrate; }
      SampleRate sampleRate() const { return _sampleRate; }
      AudioCodec codec() const { return _codec; }
      Channels channels() const { return _channels; }
      BitDepth bitDepth() const { return _bitDepth; }

    private:
      friend class TrackBuilder;

      // Property numerics
      std::chrono::milliseconds _duration{0};
      Bitrate _bitrate{};
      SampleRate _sampleRate{};
      AudioCodec _codec = AudioCodec::Unknown;
      Channels _channels{};
      BitDepth _bitDepth{};

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

    class CoverArtBuilder
    {
    public:
      struct PendingCoverArt
      {
        PictureType type = PictureType::FrontCover;
        std::variant<ResourceId, std::span<std::byte const>> source;
      };

      CoverArtBuilder& add(PictureType type, ResourceId resourceId);
      CoverArtBuilder& add(PictureType type, std::span<std::byte const> data);
      CoverArtBuilder& erase(std::size_t index);
      CoverArtBuilder& clear();

      std::vector<PendingCoverArt> const& entries() const { return _entries; }

    private:
      friend class TrackBuilder;

      std::vector<PendingCoverArt> _entries;
    };

    class CustomMetadataBuilder
    {
    public:
      CustomMetadataBuilder& add(std::string_view key, std::string_view value);
      CustomMetadataBuilder& remove(std::string_view key);
      CustomMetadataBuilder& clear();

      std::vector<std::pair<std::string_view, std::string_view>> const& pairs() const { return _customPairs; }

    private:
      friend class TrackBuilder;

      // Custom pairs - string_view pointing to external data
      std::vector<std::pair<std::string_view, std::string_view>> _customPairs;
    };

    // Sub-builder accessors - return references to stored members
    MetadataBuilder& metadata();
    MetadataBuilder const& metadata() const;

    PropertyBuilder& property();
    PropertyBuilder const& property() const;

    TagsBuilder& tags();
    TagsBuilder const& tags() const;

    CoverArtBuilder& coverArt();
    CoverArtBuilder const& coverArt() const;

    CustomMetadataBuilder& customMetadata();
    CustomMetadataBuilder const& customMetadata() const;

    // Full serialization - resolves all strings to DictionaryIds
    Result<std::pair<std::vector<std::byte>, std::vector<std::byte>>> serialize(lmdb::WriteTransaction& txn,
                                                                                DictionaryStore& dict,
                                                                                ResourceStore& resources) const;

    // Partial serialization for hot-only or cold-only updates
    Result<std::vector<std::byte>> serializeHot(lmdb::WriteTransaction& txn, DictionaryStore& dict) const;
    Result<std::vector<std::byte>> serializeCold(lmdb::WriteTransaction& txn,
                                                 DictionaryStore& dict,
                                                 ResourceStore& resources) const;

    //=============================================================================
    // Prepared structures for zero-copy serialization
    //=============================================================================

    /**
     * PreparedHot - prepared hot data for zero-copy write.
     * Resolves tag names and metadata strings to DictionaryIds before writing.
     *
     * A prepared value is an immutable snapshot: it owns every byte writeTo
     * emits and freezes the overflow-checked header lengths at prepare time,
     * so mutating or destroying builder inputs after prepare() cannot skew
     * the validated size, header fields, or payload bytes.
     */
    class PreparedHot
    {
    public:
      std::size_t size() const noexcept { return _size; }
      void writeTo(std::span<std::byte> out) const;

    private:
      PreparedHot() = default;
      static PreparedHot create(TrackBuilder const* builder, lmdb::WriteTransaction& txn, DictionaryStore& dict);

      std::string _title;
      std::vector<DictionaryId> _tagIds;
      DictionaryId _artistId = kInvalidDictionaryId;
      DictionaryId _albumId = kInvalidDictionaryId;
      DictionaryId _genreId = kInvalidDictionaryId;
      DictionaryId _albumArtistId = kInvalidDictionaryId;
      DictionaryId _composerId = kInvalidDictionaryId;
      std::uint32_t _bloomFilter = 0;
      SampleRate _sampleRate{};
      std::uint16_t _year = 0;
      std::uint16_t _titleLength = 0;
      std::uint16_t _tagLength = 0;
      BitDepth _bitDepth{};
      AudioCodec _codec{};
      std::size_t _size = 0;

      friend class TrackBuilder;
    };

    /**
     * PreparedCold - prepared cold data for zero-copy write.
     * Created by create() (handles embedded cover art, resolves custom keys).
     *
     * Like PreparedHot, a prepared value is an immutable snapshot that owns
     * every byte writeTo emits - including the URI - with all header fields
     * overflow-checked and frozen at prepare time.
     */
    class PreparedCold
    {
    public:
      std::size_t size() const noexcept { return _size; }
      void writeTo(std::span<std::byte> out) const;

    private:
      PreparedCold() = default;
      static PreparedCold create(TrackBuilder const* builder,
                                 lmdb::WriteTransaction& txn,
                                 DictionaryStore& dict,
                                 ResourceStore& resources);
      static std::vector<std::pair<DictionaryId, std::string_view>> resolveCustomMetadata(TrackBuilder const* builder,
                                                                                          lmdb::WriteTransaction& txn,
                                                                                          DictionaryStore& dict);

      void resolveClassicalIds(TrackBuilder const* builder, lmdb::WriteTransaction& txn, DictionaryStore& dict);
      void resolveCoverArt(TrackBuilder const* builder, lmdb::WriteTransaction& txn, ResourceStore& resources);
      void appendBlock(TrackColdBlockSlot slot, std::vector<std::byte> payload);
      void appendCoverArtBlock();
      void appendClassicalBlock(MetadataBuilder const& meta);
      void appendCustomMetadataBlock(std::vector<std::pair<DictionaryId, std::string_view>> const& resolvedPairs);
      void assignLayout(std::string_view uri);
      void snapshot(TrackBuilder const* builder);

      struct PreparedBlock final
      {
        TrackColdBlockSlot slot{};
        std::vector<std::byte> payload{};
      };

      std::string _uri;
      std::vector<CoverArt> _coverArt;
      std::vector<PreparedBlock> _blocks;
      std::array<std::uint16_t, kTrackColdBlockSlotCount> _blockOffsets{};
      TrackDuration _duration{};
      Bitrate _bitrate{};
      std::uint16_t _trackNumber = 0;
      std::uint16_t _trackTotal = 0;
      std::uint16_t _discNumber = 0;
      std::uint16_t _discTotal = 0;
      std::uint16_t _uriOffset = 0;
      std::uint16_t _uriLength = 0;
      DictionaryId _workId = kInvalidDictionaryId;
      DictionaryId _movementId = kInvalidDictionaryId;
      DictionaryId _conductorId = kInvalidDictionaryId;
      DictionaryId _ensembleId = kInvalidDictionaryId;
      DictionaryId _soloistId = kInvalidDictionaryId;
      Channels _channels{};
      std::size_t _size = 0;

      friend class TrackBuilder;
    };

    // Prepare methods - resolve dictionary IDs and compute sizes
    Result<std::pair<PreparedHot, PreparedCold>> prepare(lmdb::WriteTransaction& txn,
                                                         DictionaryStore& dict,
                                                         ResourceStore& resources) const;

    Result<PreparedHot> prepareHot(lmdb::WriteTransaction& txn, DictionaryStore& dict) const;

    Result<PreparedCold> prepareCold(lmdb::WriteTransaction& txn,
                                     DictionaryStore& dict,
                                     ResourceStore& resources) const;

  private:
    // Private helper methods for serialization
    static std::uint32_t computeBloomFilter(std::span<DictionaryId const> tagIds);

    // Sub-builders stored as members
    MetadataBuilder _metadataBuilder{};
    PropertyBuilder _propertyBuilder{};
    TagsBuilder _tagsBuilder{};
    CoverArtBuilder _coverArtBuilder{};
    CustomMetadataBuilder _customMetadataBuilder{};
  };
} // namespace ao::library
