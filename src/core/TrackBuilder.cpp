// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/core/TrackBuilder.h>
#include <rs/utility/ByteView.h>

#include <algorithm>

namespace rs::core
{

  constexpr std::uint32_t kBloomBitMask = 31;

  //=============================================================================
  // TrackBuilder - factory methods and proxy accessors
  //=============================================================================

  TrackBuilder TrackBuilder::createNew()
  {
    return TrackBuilder{TrackRecord{}};
  }

  TrackBuilder TrackBuilder::fromRecord(TrackRecord record)
  {
    return TrackBuilder{std::move(record)};
  }

  TrackBuilder TrackBuilder::fromView(TrackView const& view, DictionaryStore& dict)
  {
    auto record = TrackRecord{};

    if (view.isHotValid())
    {
      auto meta = view.metadata();
      record.metadata.title = std::string{meta.title()};
      record.metadata.year = meta.year();

      if (auto artistId = meta.artistId(); artistId.value() > 0)
      {
        record.metadata.artist = std::string{dict.get(artistId)};
      }
      if (auto albumId = meta.albumId(); albumId.value() > 0)
      {
        record.metadata.album = std::string{dict.get(albumId)};
      }
      if (auto albumArtistId = meta.albumArtistId(); albumArtistId.value() > 0)
      {
        record.metadata.albumArtist = std::string{dict.get(albumArtistId)};
      }
      if (auto genreId = meta.genreId(); genreId.value() > 0)
      {
        record.metadata.genre = std::string{dict.get(genreId)};
      }

      // Copy tags
      auto tagProxy = view.tags();
      auto tagCount = tagProxy.count();
      record.tags.names.reserve(tagCount);
      for (std::uint8_t i = 0; i < tagCount; ++i)
      {
        auto tagId = tagProxy.id(i);
        auto tagName = dict.get(tagId);
        record.tags.names.push_back(std::string{tagName});
      }
    }

    if (view.isColdValid())
    {
      auto prop = view.property();
      record.metadata.uri = std::string{prop.uri()};
      record.property.fileSize = prop.fileSize();
      record.property.mtime = prop.mtime();
      record.property.durationMs = prop.durationMs();
      record.property.sampleRate = prop.sampleRate();
      record.property.bitrate = prop.bitrate();
      record.property.channels = prop.channels();

      auto meta = view.metadata();
      record.metadata.coverArtId = meta.coverArtId();
      record.metadata.trackNumber = meta.trackNumber();
      record.metadata.totalTracks = meta.totalTracks();
      record.metadata.discNumber = meta.discNumber();
      record.metadata.totalDiscs = meta.totalDiscs();

      // Copy custom pairs
      for (auto const& [dictId, value] : view.custom())
      {
        auto key = dict.get(dictId);
        record.custom.pairs.emplace_back(std::string{key}, std::string{value});
      }
    }

    return TrackBuilder{std::move(record)};
  }

  TrackBuilder::TrackBuilder(TrackRecord record)
    : _record{std::move(record)}
  {
  }

  TrackBuilder::MetadataBuilder TrackBuilder::metadata()
  {
    return MetadataBuilder{*this};
  }

  TrackBuilder::PropertyBuilder TrackBuilder::property()
  {
    return PropertyBuilder{*this};
  }

  TrackBuilder::TagsBuilder TrackBuilder::tags()
  {
    return TagsBuilder{*this};
  }

  TrackBuilder::CustomBuilder TrackBuilder::custom()
  {
    return CustomBuilder{*this};
  }

  //=============================================================================
  // MetadataBuilder
  //=============================================================================

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::title(std::string v)
  {
    _builder._record.metadata.title = std::move(v);
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::uri(std::string v)
  {
    _builder._record.metadata.uri = std::move(v);
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::artist(std::string v)
  {
    _builder._record.metadata.artist = std::move(v);
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::album(std::string v)
  {
    _builder._record.metadata.album = std::move(v);
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::albumArtist(std::string v)
  {
    _builder._record.metadata.albumArtist = std::move(v);
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::genre(std::string v)
  {
    _builder._record.metadata.genre = std::move(v);
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::year(std::uint16_t v)
  {
    _builder._record.metadata.year = v;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::trackNumber(std::uint16_t v)
  {
    _builder._record.metadata.trackNumber = v;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::totalTracks(std::uint16_t v)
  {
    _builder._record.metadata.totalTracks = v;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::discNumber(std::uint16_t v)
  {
    _builder._record.metadata.discNumber = v;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::totalDiscs(std::uint16_t v)
  {
    _builder._record.metadata.totalDiscs = v;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::coverArtId(std::uint32_t v)
  {
    _builder._record.metadata.coverArtId = v;
    return *this;
  }

  //=============================================================================
  // PropertyBuilder
  //=============================================================================

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::fileSize(std::uint64_t v)
  {
    _builder._record.property.fileSize = v;
    return *this;
  }

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::mtime(std::uint64_t v)
  {
    _builder._record.property.mtime = v;
    return *this;
  }

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::durationMs(std::uint32_t v)
  {
    _builder._record.property.durationMs = v;
    return *this;
  }

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::bitrate(std::uint32_t v)
  {
    _builder._record.property.bitrate = v;
    return *this;
  }

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::sampleRate(std::uint32_t v)
  {
    _builder._record.property.sampleRate = v;
    return *this;
  }

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::codecId(std::uint16_t v)
  {
    _builder._record.property.codecId = v;
    return *this;
  }

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::channels(std::uint8_t v)
  {
    _builder._record.property.channels = v;
    return *this;
  }

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::bitDepth(std::uint8_t v)
  {
    _builder._record.property.bitDepth = v;
    return *this;
  }

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::rating(std::uint8_t v)
  {
    _builder._record.property.rating = v;
    return *this;
  }

  //=============================================================================
  // TagsBuilder
  //=============================================================================

  TrackBuilder::TagsBuilder& TrackBuilder::TagsBuilder::add(std::string tagName)
  {
    _builder._record.tags.names.push_back(std::move(tagName));
    return *this;
  }

  TrackBuilder::TagsBuilder& TrackBuilder::TagsBuilder::remove(std::string tagName)
  {
    std::erase(_builder._record.tags.names, tagName);
    return *this;
  }

  TrackBuilder::TagsBuilder& TrackBuilder::TagsBuilder::clear()
  {
    _builder._record.tags.names.clear();
    return *this;
  }

  std::vector<std::string> const& TrackBuilder::TagsBuilder::names() const
  {
    return _builder._record.tags.names;
  }

  //=============================================================================
  // CustomBuilder
  //=============================================================================

  TrackBuilder::CustomBuilder& TrackBuilder::CustomBuilder::add(std::string key, std::string value)
  {
    _builder._record.custom.pairs.emplace_back(std::move(key), std::move(value));
    return *this;
  }

  TrackBuilder::CustomBuilder& TrackBuilder::CustomBuilder::remove(std::string key)
  {
    std::erase_if(_builder._record.custom.pairs, [&key](auto const& pair) { return pair.first == key; });
    return *this;
  }

  TrackBuilder::CustomBuilder& TrackBuilder::CustomBuilder::clear()
  {
    _builder._record.custom.pairs.clear();
    return *this;
  }

  std::vector<std::pair<std::string, std::string>> const& TrackBuilder::CustomBuilder::pairs() const
  {
    return _builder._record.custom.pairs;
  }

  //=============================================================================
  // TrackBuilder - serialization
  //=============================================================================

  std::uint32_t TrackBuilder::computeBloomFilter(std::span<DictionaryId const> tagIds)
  {
    std::uint32_t bloom = 0;
    for (auto tagId : tagIds) { bloom |= (std::uint32_t{1} << (tagId.value() & kBloomBitMask)); }
    return bloom;
  }

  TrackHotHeader TrackBuilder::buildHotHeader(DictionaryStore& dict,
                                              lmdb::WriteTransaction& txn,
                                              std::span<DictionaryId const> tagIds) const
  {
    auto artistId = dict.put(txn, _record.metadata.artist);
    auto albumId = dict.put(txn, _record.metadata.album);
    auto genreId = dict.put(txn, _record.metadata.genre);
    auto albumArtistId = dict.put(txn, _record.metadata.albumArtist);

    return TrackHotHeader{
      .tagBloom = computeBloomFilter(tagIds),
      .artistId = artistId,
      .albumId = albumId,
      .genreId = genreId,
      .albumArtistId = albumArtistId,
      .year = _record.metadata.year,
      .codecId = _record.property.codecId,
      .bitDepth = _record.property.bitDepth,
      .titleLen = static_cast<std::uint16_t>(_record.metadata.title.size()),
      .tagLen = static_cast<std::uint16_t>(tagIds.size() * sizeof(DictionaryId)),
      .rating = _record.property.rating,
      .padding = std::byte{0},
    };
  }

  TrackColdHeader TrackBuilder::buildColdHeader(std::size_t customCount,
                                                std::uint16_t uriOffset,
                                                std::uint16_t uriLen) const
  {
    auto [fileSizeLo, fileSizeHi] = utility::splitInt64(_record.property.fileSize);
    auto [mtimeLo, mtimeHi] = utility::splitInt64(_record.property.mtime);

    return TrackColdHeader{
      .fileSizeLo = fileSizeLo,
      .fileSizeHi = fileSizeHi,
      .mtimeLo = mtimeLo,
      .mtimeHi = mtimeHi,
      .durationMs = _record.property.durationMs,
      .sampleRate = _record.property.sampleRate,
      .coverArtId = _record.metadata.coverArtId,
      .bitrate = _record.property.bitrate,
      .trackNumber = _record.metadata.trackNumber,
      .totalTracks = _record.metadata.totalTracks,
      .discNumber = _record.metadata.discNumber,
      .totalDiscs = _record.metadata.totalDiscs,
      .customCount = static_cast<std::uint16_t>(customCount),
      .uriOffset = uriOffset,
      .uriLen = uriLen,
      .channels = _record.property.channels,
      .padding = std::byte{0},
    };
  }

  std::vector<std::byte> TrackBuilder::serializeHot(DictionaryStore& dict, lmdb::WriteTransaction& txn) const
  {
    // Resolve tag names to DictionaryIds
    auto tagIds = std::vector<DictionaryId>{};
    tagIds.reserve(_record.tags.names.size());
    for (auto const& name : _record.tags.names) { tagIds.push_back(dict.put(txn, name)); }

    std::vector<std::byte> data;

    // Build header with resolved dictionary IDs
    auto hdr = buildHotHeader(dict, txn, tagIds);

    // Write header
    auto headerBytes = utility::asBytes(hdr);
    data.insert_range(data.end(), headerBytes);

    // Write tags: 4-byte tag IDs
    for (auto tagId : tagIds)
    {
      auto idBytes = utility::asBytes(tagId);
      data.insert_range(data.end(), idBytes);
    }

    // Write title (null-terminated)
    auto titleBytes = utility::asBytes(_record.metadata.title);
    data.insert_range(data.end(), titleBytes);
    data.push_back(static_cast<std::byte>('\0'));

    // Pad to 4-byte alignment
    while (data.size() % 4 != 0) { data.push_back(std::byte{0}); }

    return data;
  }

  std::vector<std::byte> TrackBuilder::serializeCold(DictionaryStore& dict, lmdb::WriteTransaction& txn) const
  {
    // Resolve custom keys to DictionaryIds
    auto resolvedPairs = std::vector<std::pair<DictionaryId, std::string>>{};
    resolvedPairs.reserve(_record.custom.pairs.size());

    for (auto const& [key, value] : _record.custom.pairs)
    {
      auto dictId = dict.put(txn, key);
      resolvedPairs.emplace_back(dictId, value);
    }

    // Sort by dictId for binary search
    std::ranges::sort(resolvedPairs, {}, &std::pair<DictionaryId, std::string>::first);

    constexpr std::size_t kEntrySize = 8; // dictId(4) + offset(2) + len(2)
    std::size_t entryCount = resolvedPairs.size();
    std::size_t totalValueSize = 0;
    for (auto const& [_, value] : resolvedPairs) { totalValueSize += value.size(); }

    std::uint16_t uriLen = static_cast<std::uint16_t>(_record.metadata.uri.size());
    auto result = std::vector<std::byte>{};
    result.reserve(sizeof(TrackColdHeader) + (entryCount * kEntrySize) + totalValueSize + uriLen + 4);

    // Build header
    std::uint16_t uriOffset =
      static_cast<std::uint16_t>((sizeof(TrackColdHeader) + entryCount * kEntrySize) + totalValueSize);
    auto hdr = buildColdHeader(entryCount, uriOffset, uriLen);
    result.insert_range(result.end(), utility::asBytes(hdr));

    // Value data starts after header + entries
    std::size_t valueOffset = (entryCount * kEntrySize) + sizeof(TrackColdHeader);

    // Write entries with calculated offsets
    for (auto const& [dictId, value] : resolvedPairs)
    {
      auto valueLen = static_cast<std::uint16_t>(value.size());
      result.insert_range(result.end(), utility::asBytes(dictId.value()));
      result.insert_range(result.end(), utility::asBytes(static_cast<std::uint16_t>(valueOffset)));
      result.insert_range(result.end(), utility::asBytes(valueLen));
      valueOffset += valueLen;
    }

    // Write all values contiguously
    for (auto const& [_, value] : resolvedPairs) { result.insert_range(result.end(), utility::asBytes(value)); }

    // Write uri (null-terminated)
    result.insert_range(result.end(), utility::asBytes(_record.metadata.uri));
    result.push_back(std::byte{'\0'});

    // Pad to 4-byte alignment
    while (result.size() % 4 != 0) { result.push_back(std::byte{0}); }

    return result;
  }

  std::pair<std::vector<std::byte>, std::vector<std::byte>> TrackBuilder::serialize(DictionaryStore& dict,
                                                                                    lmdb::WriteTransaction& txn) const
  {
    return {serializeHot(dict, txn), serializeCold(dict, txn)};
  }

} // namespace rs::core
