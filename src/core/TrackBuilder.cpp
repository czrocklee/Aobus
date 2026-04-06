// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/core/TrackBuilder.h>
#include <rs/utility/ByteView.h>

#include <algorithm>

namespace rs::core
{

  constexpr std::uint32_t kBloomBitMask = 31;

  //=============================================================================
  // TrackBuilder - factory methods
  //=============================================================================

  TrackBuilder TrackBuilder::createNew()
  {
    return TrackBuilder{};
  }

  TrackBuilder TrackBuilder::fromRecord(TrackRecord const& record)
  {
    auto builder = TrackBuilder{};

    // Extract string_view from record's owned strings
    auto& meta = builder._metadataBuilder;
    meta._title = record.metadata.title;
    meta._artist = record.metadata.artist;
    meta._album = record.metadata.album;
    meta._albumArtist = record.metadata.albumArtist;
    meta._genre = record.metadata.genre;
    meta._year = record.metadata.year;
    meta._trackNumber = record.metadata.trackNumber;
    meta._totalTracks = record.metadata.totalTracks;
    meta._discNumber = record.metadata.discNumber;
    meta._totalDiscs = record.metadata.totalDiscs;
    meta._coverArtId = record.metadata.coverArtId;
    meta._rating = record.metadata.rating;

    auto& prop = builder._propertyBuilder;
    prop._uri = record.property.uri;
    prop._fileSize = record.property.fileSize;
    prop._mtime = record.property.mtime;
    prop._durationMs = record.property.durationMs;
    prop._bitrate = record.property.bitrate;
    prop._sampleRate = record.property.sampleRate;
    prop._codecId = record.property.codecId;
    prop._channels = record.property.channels;
    prop._bitDepth = record.property.bitDepth;

    auto& tags = builder._tagsBuilder;
    tags._tagNames.reserve(record.tags.names.size());

    for (auto const& name : record.tags.names)
    {
      tags._tagNames.push_back(name);
    }

    auto& custom = builder._customBuilder;
    custom._customPairs.reserve(record.custom.pairs.size());

    for (auto const& [key, value] : record.custom.pairs)
    {
      custom._customPairs.emplace_back(key, value);
    }

    return builder;
  }

  TrackBuilder TrackBuilder::fromView(TrackView const& view, DictionaryStore& dict)
  {
    auto builder = TrackBuilder{};

    if (view.isHotValid())
    {
      auto meta = view.metadata();
      builder._metadataBuilder._title = meta.title();
      builder._metadataBuilder._year = meta.year();

      if (auto artistId = meta.artistId(); artistId.value() > 0)
      {
        builder._metadataBuilder._artist = dict.get(artistId);
      }

      if (auto albumId = meta.albumId(); albumId.value() > 0)
      {
        builder._metadataBuilder._album = dict.get(albumId);
      }

      if (auto albumArtistId = meta.albumArtistId(); albumArtistId.value() > 0)
      {
        builder._metadataBuilder._albumArtist = dict.get(albumArtistId);
      }

      if (auto genreId = meta.genreId(); genreId.value() > 0)
      {
        builder._metadataBuilder._genre = dict.get(genreId);
      }

      builder._tagsBuilder._tagNames.reserve(view.tags().count());
      
      for (auto tagId : view.tags())
      {
        builder._tagsBuilder._tagNames.push_back(dict.get(tagId));
      }
    }

    if (view.isColdValid())
    {
      auto prop = view.property();
      builder._propertyBuilder._uri = prop.uri();
      builder._propertyBuilder._fileSize = prop.fileSize();
      builder._propertyBuilder._mtime = prop.mtime();
      builder._propertyBuilder._durationMs = prop.durationMs();
      builder._propertyBuilder._sampleRate = prop.sampleRate();
      builder._propertyBuilder._bitrate = prop.bitrate();
      builder._propertyBuilder._channels = prop.channels();

      auto meta = view.metadata();
      builder._metadataBuilder._coverArtId = meta.coverArtId();
      builder._metadataBuilder._trackNumber = meta.trackNumber();
      builder._metadataBuilder._totalTracks = meta.totalTracks();
      builder._metadataBuilder._discNumber = meta.discNumber();
      builder._metadataBuilder._totalDiscs = meta.totalDiscs();

      // Copy custom pairs
      for (auto const& [dictId, value] : view.custom())
      {
        auto key = dict.get(dictId);
        builder._customBuilder._customPairs.emplace_back(key, value);
      }
    }

    return builder;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::metadata()
  {
    return _metadataBuilder;
  }

  TrackBuilder::PropertyBuilder& TrackBuilder::property()
  {
    return _propertyBuilder;
  }

  TrackBuilder::TagsBuilder& TrackBuilder::tags()
  {
    return _tagsBuilder;
  }

  TrackBuilder::CustomBuilder& TrackBuilder::custom()
  {
    return _customBuilder;
  }

  //=============================================================================
  // TrackBuilder::record() - constructs TrackRecord on-the-fly
  //=============================================================================

  TrackRecord TrackBuilder::record() const
  {
    auto record = TrackRecord{};
    auto const& meta = _metadataBuilder;
    auto const& prop = _propertyBuilder;

    // Copy string_view data into owned strings in record
    record.metadata.title = std::string{meta._title};
    record.metadata.artist = std::string{meta._artist};
    record.metadata.album = std::string{meta._album};
    record.metadata.albumArtist = std::string{meta._albumArtist};
    record.metadata.genre = std::string{meta._genre};

    record.metadata.year = meta._year;
    record.metadata.trackNumber = meta._trackNumber;
    record.metadata.totalTracks = meta._totalTracks;
    record.metadata.discNumber = meta._discNumber;
    record.metadata.totalDiscs = meta._totalDiscs;
    record.metadata.coverArtId = meta._coverArtId;

    record.property.uri = std::string{prop._uri};
    record.property.fileSize = prop._fileSize;
    record.property.mtime = prop._mtime;
    record.property.durationMs = prop._durationMs;
    record.property.bitrate = prop._bitrate;
    record.property.sampleRate = prop._sampleRate;
    record.property.codecId = prop._codecId;
    record.property.channels = prop._channels;
    record.property.bitDepth = prop._bitDepth;
    record.metadata.rating = meta._rating;

    for (auto const& name : _tagsBuilder._tagNames)
    {
      record.tags.names.push_back(std::string{name});
    }

    for (auto const& [key, value] : _customBuilder._customPairs)
    {
      record.custom.pairs.emplace_back(std::string{key}, std::string{value});
    }

    return record;
  }

  //=============================================================================
  // MetadataBuilder
  //=============================================================================

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::title(std::string_view v)
  {
    _title = v;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::artist(std::string_view v)
  {
    _artist = v;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::album(std::string_view v)
  {
    _album = v;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::albumArtist(std::string_view v)
  {
    _albumArtist = v;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::genre(std::string_view v)
  {
    _genre = v;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::year(std::uint16_t v)
  {
    _year = v;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::trackNumber(std::uint16_t v)
  {
    _trackNumber = v;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::totalTracks(std::uint16_t v)
  {
    _totalTracks = v;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::discNumber(std::uint16_t v)
  {
    _discNumber = v;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::totalDiscs(std::uint16_t v)
  {
    _totalDiscs = v;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::coverArtId(std::uint32_t v)
  {
    _coverArtId = v;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::rating(std::uint8_t v)
  {
    _rating = v;
    return *this;
  }

  //=============================================================================
  // PropertyBuilder
  //=============================================================================

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::fileSize(std::uint64_t v)
  {
    _fileSize = v;
    return *this;
  }

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::mtime(std::uint64_t v)
  {
    _mtime = v;
    return *this;
  }

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::durationMs(std::uint32_t v)
  {
    _durationMs = v;
    return *this;
  }

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::bitrate(std::uint32_t v)
  {
    _bitrate = v;
    return *this;
  }

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::sampleRate(std::uint32_t v)
  {
    _sampleRate = v;
    return *this;
  }

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::codecId(std::uint16_t v)
  {
    _codecId = v;
    return *this;
  }

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::channels(std::uint8_t v)
  {
    _channels = v;
    return *this;
  }

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::bitDepth(std::uint8_t v)
  {
    _bitDepth = v;
    return *this;
  }

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::uri(std::string_view v)
  {
    _uri = v;
    return *this;
  }

  //=============================================================================
  // TagsBuilder
  //=============================================================================

  TrackBuilder::TagsBuilder& TrackBuilder::TagsBuilder::add(std::string_view tagName)
  {
    _tagNames.push_back(tagName);
    return *this;
  }

  TrackBuilder::TagsBuilder& TrackBuilder::TagsBuilder::remove(std::string_view tagName)
  {
    std::erase(_tagNames, tagName);
    return *this;
  }

  TrackBuilder::TagsBuilder& TrackBuilder::TagsBuilder::clear()
  {
    _tagNames.clear();
    return *this;
  }

  //=============================================================================
  // CustomBuilder
  //=============================================================================

  TrackBuilder::CustomBuilder& TrackBuilder::CustomBuilder::add(std::string_view key, std::string_view value)
  {
    _customPairs.emplace_back(key, value);
    return *this;
  }

  TrackBuilder::CustomBuilder& TrackBuilder::CustomBuilder::remove(std::string_view key)
  {
    std::erase_if(_customPairs, [&key](auto const& pair) { return pair.first == key; });
    return *this;
  }

  TrackBuilder::CustomBuilder& TrackBuilder::CustomBuilder::clear()
  {
    _customPairs.clear();
    return *this;
  }

  //=============================================================================
  // TrackBuilder - serialization
  //=============================================================================

  std::uint32_t TrackBuilder::computeBloomFilter(std::span<DictionaryId const> tagIds)
  {
    std::uint32_t bloom = 0;
    for (auto tagId : tagIds)
    {
      bloom |= (std::uint32_t{1} << (tagId.value() & kBloomBitMask));
    }
    return bloom;
  }

  TrackHotHeader TrackBuilder::buildHotHeader(DictionaryStore& dict,
                                              lmdb::WriteTransaction& txn,
                                              std::span<DictionaryId const> tagIds) const
  {
    auto const& meta = _metadataBuilder;
    auto const& prop = _propertyBuilder;

    auto artistId = dict.put(txn, meta._artist);
    auto albumId = dict.put(txn, meta._album);
    auto genreId = dict.put(txn, meta._genre);
    auto albumArtistId = dict.put(txn, meta._albumArtist);

    return TrackHotHeader{
      .tagBloom = computeBloomFilter(tagIds),
      .artistId = artistId,
      .albumId = albumId,
      .genreId = genreId,
      .albumArtistId = albumArtistId,
      .year = meta._year,
      .codecId = prop._codecId,
      .bitDepth = prop._bitDepth,
      .titleLen = static_cast<std::uint16_t>(meta._title.size()),
      .tagLen = static_cast<std::uint16_t>(tagIds.size() * sizeof(DictionaryId)),
      .rating = meta._rating,
      .padding = std::byte{0},
    };
  }

  TrackColdHeader TrackBuilder::buildColdHeader(std::size_t customCount,
                                                std::uint16_t uriOffset,
                                                std::uint16_t uriLen) const
  {
    auto const& meta = _metadataBuilder;
    auto const& prop = _propertyBuilder;

    auto [fileSizeLo, fileSizeHi] = utility::splitInt64(prop._fileSize);
    auto [mtimeLo, mtimeHi] = utility::splitInt64(prop._mtime);

    return TrackColdHeader{
      .fileSizeLo = fileSizeLo,
      .fileSizeHi = fileSizeHi,
      .mtimeLo = mtimeLo,
      .mtimeHi = mtimeHi,
      .durationMs = prop._durationMs,
      .sampleRate = prop._sampleRate,
      .coverArtId = meta._coverArtId,
      .bitrate = prop._bitrate,
      .trackNumber = meta._trackNumber,
      .totalTracks = meta._totalTracks,
      .discNumber = meta._discNumber,
      .totalDiscs = meta._totalDiscs,
      .customCount = static_cast<std::uint16_t>(customCount),
      .uriOffset = uriOffset,
      .uriLen = uriLen,
      .channels = prop._channels,
      .padding = std::byte{0},
    };
  }

  std::vector<std::byte> TrackBuilder::serializeHot(DictionaryStore& dict, lmdb::WriteTransaction& txn) const
  {
    // Resolve tag names to DictionaryIds
    auto tagIds = std::vector<DictionaryId>{};
    tagIds.reserve(_tagsBuilder._tagNames.size());

    for (auto const& name : _tagsBuilder._tagNames)
    {
      tagIds.push_back(dict.put(txn, name));
    }

    auto data = std::vector<std::byte>{};

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
    auto titleBytes = utility::asBytes(_metadataBuilder._title);
    data.insert_range(data.end(), titleBytes);
    data.push_back(static_cast<std::byte>('\0'));

    // Pad to 4-byte alignment
    while (data.size() % 4 != 0)
    {
      data.push_back(std::byte{0});
    }

    return data;
  }

  std::vector<std::byte> TrackBuilder::serializeCold(DictionaryStore& dict, lmdb::WriteTransaction& txn) const
  {
    // Resolve custom keys to DictionaryIds
    auto resolvedPairs = std::vector<std::pair<DictionaryId, std::string>>{};
    resolvedPairs.reserve(_customBuilder._customPairs.size());

    for (auto const& [key, value] : _customBuilder._customPairs)
    {
      auto dictId = dict.put(txn, key);
      resolvedPairs.emplace_back(dictId, std::string{value});
    }

    // Sort by dictId for binary search
    std::ranges::sort(resolvedPairs, {}, &std::pair<DictionaryId, std::string>::first);

    constexpr std::size_t kEntrySize = 8; // dictId(4) + offset(2) + len(2)
    std::size_t entryCount = resolvedPairs.size();
    std::size_t totalValueSize = 0;

    for (auto const& [_, value] : resolvedPairs)
    {
      totalValueSize += value.size();
    }

    auto uriLen = static_cast<std::uint16_t>(_propertyBuilder._uri.size());
    auto result = std::vector<std::byte>{};
    result.reserve(sizeof(TrackColdHeader) + (entryCount * kEntrySize) + totalValueSize + uriLen + 4);

    // Build header
    auto uriOffset = static_cast<std::uint16_t>((sizeof(TrackColdHeader) + entryCount * kEntrySize) + totalValueSize);
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
    for (auto const& [_, value] : resolvedPairs)
    {
      result.insert_range(result.end(), utility::asBytes(value));
    }

    // Write uri (null-terminated)
    result.insert_range(result.end(), utility::asBytes(_propertyBuilder._uri));
    result.push_back(std::byte{'\0'});

    // Pad to 4-byte alignment
    while (result.size() % 4 != 0)
    {
      result.push_back(std::byte{0});
    }

    return result;
  }

  std::pair<std::vector<std::byte>, std::vector<std::byte>> TrackBuilder::serialize(DictionaryStore& dict,
                                                                                    lmdb::WriteTransaction& txn) const
  {
    return {serializeHot(dict, txn), serializeCold(dict, txn)};
  }

} // namespace rs::core