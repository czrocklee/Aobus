// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/core/TrackBuilder.h>
#include <rs/utility/ByteView.h>

#include <algorithm>
#include <cassert>
#include <cstring>

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
    record.metadata.rating = meta._rating;

    record.property.uri = std::string{prop._uri};
    record.property.fileSize = prop._fileSize;
    record.property.mtime = prop._mtime;
    record.property.durationMs = prop._durationMs;
    record.property.bitrate = prop._bitrate;
    record.property.sampleRate = prop._sampleRate;
    record.property.codecId = prop._codecId;
    record.property.channels = prop._channels;
    record.property.bitDepth = prop._bitDepth;

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
    _embeddedCoverArt = {};
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::coverArtData(std::span<std::byte const> v)
  {
    _embeddedCoverArt = v;
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

  std::vector<std::byte> TrackBuilder::serializeHot(lmdb::WriteTransaction& txn, DictionaryStore& dict)
  {
    auto prepared = PreparedHot{this, txn, dict};
    auto result = std::vector<std::byte>(prepared.size());
    prepared.writeTo(result);
    return result;
  }

  std::vector<std::byte> TrackBuilder::serializeCold(lmdb::WriteTransaction& txn,
                                                     DictionaryStore& dict,
                                                     ResourceStore& resources)
  {
    auto prepared = PreparedCold{this, txn, dict, resources};
    auto result = std::vector<std::byte>(prepared.size());
    prepared.writeTo(result);
    return result;
  }

  std::pair<std::vector<std::byte>, std::vector<std::byte>> TrackBuilder::serialize(lmdb::WriteTransaction& txn,
                                                                                    DictionaryStore& dict,
                                                                                    ResourceStore& resources)
  {
    return {serializeHot(txn, dict), serializeCold(txn, dict, resources)};
  }

  //=============================================================================
  // Prepared structures for zero-copy serialization
  //=============================================================================

  TrackBuilder::PreparedHot::PreparedHot(TrackBuilder const* builder,
                                         lmdb::WriteTransaction& txn,
                                         DictionaryStore& dict)
    : _builder{builder}
  {
    // Resolve tag names to DictionaryIds
    _tagIds.reserve(builder->_tagsBuilder._tagNames.size());

    for (auto const& name : builder->_tagsBuilder._tagNames)
    {
      _tagIds.push_back(dict.put(txn, name));
    }

    // Resolve metadata strings to DictionaryIds for header
    if (!builder->_metadataBuilder._artist.empty())
    {
      _artistId = dict.put(txn, builder->_metadataBuilder._artist);
    }

    if (!builder->_metadataBuilder._album.empty())
    {
      _albumId = dict.put(txn, builder->_metadataBuilder._album);
    }

    if (!builder->_metadataBuilder._genre.empty())
    {
      _genreId = dict.put(txn, builder->_metadataBuilder._genre);
    }

    if (!builder->_metadataBuilder._albumArtist.empty())
    {
      _albumArtistId = dict.put(txn, builder->_metadataBuilder._albumArtist);
    }

    _bloomFilter = computeBloomFilter(_tagIds);

    // Compute total hot size: Header(32) + tags + title
    _size = sizeof(TrackHotHeader);
    _size += _tagIds.size() * sizeof(DictionaryId);
    _size += builder->_metadataBuilder._title.size();
    _size = (_size + 3) & ~3; // pad to 4 bytes
  }

  TrackBuilder::PreparedCold::PreparedCold(TrackBuilder const* builder,
                                           lmdb::WriteTransaction& txn,
                                           DictionaryStore& dict,
                                           ResourceStore& resources)
    : _builder{builder}
  {
    // Handle embedded cover art - store resolved ID in PreparedCold
    if (!_builder->_metadataBuilder._embeddedCoverArt.empty())
    {
      auto writer = resources.writer(txn);
      _coverArtId = writer.create(_builder->_metadataBuilder._embeddedCoverArt).value();
    }
    else
    {
      _coverArtId = _builder->_metadataBuilder._coverArtId;
    }

    // Resolve custom keys to DictionaryIds
    _resolvedPairs.reserve(_builder->_customBuilder._customPairs.size());

    for (auto const& [key, value] : _builder->_customBuilder._customPairs)
    {
      auto dictId = dict.put(txn, key);
      _resolvedPairs.emplace_back(dictId, value);
    }

    // Sort by dictId for binary search
    std::ranges::sort(_resolvedPairs, {}, &std::pair<DictionaryId, std::string_view>::first);

    // Compute sizes
    std::size_t entryCount = _resolvedPairs.size();
    std::size_t totalValueSize = 0;

    for (auto const& [_, value] : _resolvedPairs)
    {
      totalValueSize += value.size();
    }

    _uriLen = static_cast<std::uint16_t>(_builder->_propertyBuilder._uri.size());

    // Cold layout: header(48) + entries(N*8) + values + uri
    std::size_t size = sizeof(TrackColdHeader);
    size += entryCount * 8; // 8 bytes per entry
    size += totalValueSize;
    size += _uriLen;
    size = (size + 3) & ~3; // pad to 4 bytes

    _uriOffset = static_cast<std::uint16_t>(sizeof(TrackColdHeader) + entryCount * 8 + totalValueSize);
    _size = size;
  }

  std::pair<TrackBuilder::PreparedHot, TrackBuilder::PreparedCold> TrackBuilder::prepare(lmdb::WriteTransaction& txn,
                                                                                         DictionaryStore& dict,
                                                                                         ResourceStore& resources)
  {
    return {PreparedHot{this, txn, dict}, PreparedCold{this, txn, dict, resources}};
  }

  TrackBuilder::PreparedHot TrackBuilder::prepareHot(lmdb::WriteTransaction& txn, DictionaryStore& dict) const
  {
    return PreparedHot{this, txn, dict};
  }

  TrackBuilder::PreparedCold TrackBuilder::prepareCold(lmdb::WriteTransaction& txn,
                                                       DictionaryStore& dict,
                                                       ResourceStore& resources) const
  {
    return PreparedCold{this, txn, dict, resources};
  }

  void TrackBuilder::PreparedHot::writeTo(std::span<std::byte> out) const
  {
    auto& builder = *_builder;

    // Exact size validation and alignment check
    assert(out.size() == _size && "PreparedHot::writeTo: size mismatch");
    assert(reinterpret_cast<std::uintptr_t>(out.data()) % 4 == 0 && "out must be 4-byte aligned");

    auto pos = std::size_t{0};

    // Write header (4-byte aligned)
    *(reinterpret_cast<TrackHotHeader*>(out.data())) = TrackHotHeader{
      .tagBloom = _bloomFilter,
      .artistId = _artistId,
      .albumId = _albumId,
      .genreId = _genreId,
      .albumArtistId = _albumArtistId,
      .year = builder._metadataBuilder._year,
      .codecId = builder._propertyBuilder._codecId,
      .bitDepth = builder._propertyBuilder._bitDepth,
      .titleLen = static_cast<std::uint16_t>(builder._metadataBuilder._title.size()),
      .tagLen = static_cast<std::uint16_t>(_tagIds.size() * sizeof(DictionaryId)),
      .rating = builder._metadataBuilder._rating,
      .padding = std::byte{0},
    };

    pos += sizeof(TrackHotHeader);

    // Write tags: 4-byte tag IDs (aligned)
    std::memcpy(out.data() + pos, _tagIds.data(), _tagIds.size() * sizeof(DictionaryId));
    pos += _tagIds.size() * sizeof(DictionaryId);

    // Write title
    if (!builder._metadataBuilder._title.empty())
    {
      std::memcpy(out.data() + pos, builder._metadataBuilder._title.data(), builder._metadataBuilder._title.size());
    }

    pos += builder._metadataBuilder._title.size();

    // Pad to 4 bytes
    while (pos % 4 != 0)
    {
      out[pos++] = std::byte{0};
    }
  }

  void TrackBuilder::PreparedCold::writeTo(std::span<std::byte> out) const
  {
    auto const& meta = _builder->_metadataBuilder;
    auto const& prop = _builder->_propertyBuilder;

    // Exact size validation and alignment check
    assert(out.size() == _size && "PreparedCold::writeTo: size mismatch");
    assert(reinterpret_cast<std::uintptr_t>(out.data()) % 4 == 0 && "out must be 4-byte aligned");

    auto pos = std::size_t{0};

    // Write header directly (4-byte aligned)
    auto* hdrOut = reinterpret_cast<TrackColdHeader*>(out.data());
    {
      auto [fileSizeLo, fileSizeHi] = utility::splitInt64(prop._fileSize);
      auto [mtimeLo, mtimeHi] = utility::splitInt64(prop._mtime);

      *hdrOut = TrackColdHeader{
        .fileSizeLo = fileSizeLo,
        .fileSizeHi = fileSizeHi,
        .mtimeLo = mtimeLo,
        .mtimeHi = mtimeHi,
        .durationMs = prop._durationMs,
        .sampleRate = prop._sampleRate,
        .coverArtId = _coverArtId,
        .bitrate = prop._bitrate,
        .trackNumber = meta._trackNumber,
        .totalTracks = meta._totalTracks,
        .discNumber = meta._discNumber,
        .totalDiscs = meta._totalDiscs,
        .customCount = static_cast<std::uint16_t>(_resolvedPairs.size()),
        .uriOffset = _uriOffset,
        .uriLen = _uriLen,
        .channels = prop._channels,
        .padding = std::byte{0},
      };
    }
    pos += sizeof(TrackColdHeader);

    // Compute value data offset
    std::size_t valueOffset = sizeof(TrackColdHeader) + _resolvedPairs.size() * 8;

    // Write entries: dictId(4) + offset(2) + len(2) each, all 4-byte aligned
    for (auto const& [dictId, value] : _resolvedPairs)
    {
      auto valueLen = static_cast<std::uint16_t>(value.size());

      // dictId at pos (4-byte aligned)
      auto* dictIdOut = reinterpret_cast<DictionaryId*>(out.data() + pos);
      *dictIdOut = dictId;
      pos += sizeof(DictionaryId);

      // offset at pos (4-byte aligned)
      auto* offsetOut = reinterpret_cast<std::uint16_t*>(out.data() + pos);
      *offsetOut = static_cast<std::uint16_t>(valueOffset);
      pos += sizeof(std::uint16_t);

      // len at pos (4-byte aligned)
      auto* lenOut = reinterpret_cast<std::uint16_t*>(out.data() + pos);
      *lenOut = valueLen;
      pos += sizeof(std::uint16_t);

      valueOffset += valueLen;
    }

    // Write all values contiguously
    for (auto const& [_, value] : _resolvedPairs)
    {
      if (!value.empty())
      {
        std::memcpy(out.data() + pos, value.data(), value.size());
      }
      pos += value.size();
    }

    // Write uri
    if (_uriLen > 0)
    {
      std::memcpy(out.data() + pos, _builder->_propertyBuilder._uri.data(), _uriLen);
    }
    pos += _uriLen;

    // Pad to 4 bytes
    while (pos % 4 != 0)
    {
      out[pos++] = std::byte{0};
    }
  }

} // namespace rs::core