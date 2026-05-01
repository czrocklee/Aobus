// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/library/TrackBuilder.h>
#include <rs/utility/ByteView.h>

#include <algorithm>
#include <cstring>
#include <gsl-lite/gsl-lite.hpp>

namespace rs::library
{
  constexpr std::uint32_t kBloomBitMask = 31;

  //=============================================================================
  // TrackBuilder - factory methods
  //=============================================================================

  TrackBuilder TrackBuilder::createNew()
  {
    return TrackBuilder{};
  }

  TrackBuilder TrackBuilder::fromView(TrackView const& view, DictionaryStore& dict)
  {
    auto builder = TrackBuilder{};

    if (view.isHotValid())
    {
      auto meta = view.metadata();
      builder.metadata().title(meta.title()).year(meta.year());

      if (auto artistId = meta.artistId(); artistId.value() > 0)
      {
        builder.metadata().artist(dict.get(artistId));
      }

      if (auto albumId = meta.albumId(); albumId.value() > 0)
      {
        builder.metadata().album(dict.get(albumId));
      }

      if (auto albumArtistId = meta.albumArtistId(); albumArtistId.value() > 0)
      {
        builder.metadata().albumArtist(dict.get(albumArtistId));
      }

      if (auto composerId = meta.composerId(); composerId.value() > 0)
      {
        builder.metadata().composer(dict.get(composerId));
      }

      if (auto genreId = meta.genreId(); genreId.value() > 0)
      {
        builder.metadata().genre(dict.get(genreId));
      }

      for (auto tagId : view.tags())
      {
        builder.tags().add(dict.get(tagId));
      }
    }

    if (view.isColdValid())
    {
      auto prop = view.property();
      builder.property()
        .uri(prop.uri())
        .fileSize(prop.fileSize())
        .mtime(prop.mtime())
        .durationMs(prop.durationMs())
        .sampleRate(prop.sampleRate())
        .bitrate(prop.bitrate())
        .channels(prop.channels());

      auto meta = view.metadata();
      builder.metadata()
        .coverArtId(meta.coverArtId())
        .trackNumber(meta.trackNumber())
        .totalTracks(meta.totalTracks())
        .discNumber(meta.discNumber())
        .totalDiscs(meta.totalDiscs());

      if (auto workId = meta.workId(); workId.value() > 0)
      {
        builder.metadata().work(dict.get(workId));
      }

      for (auto const& [dictId, value] : view.custom())
      {
        builder.custom().add(dict.get(dictId), value);
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
  // MetadataBuilder
  //=============================================================================

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::title(std::string_view val)
  {
    _title = val;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::artist(std::string_view val)
  {
    _artist = val;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::album(std::string_view val)
  {
    _album = val;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::albumArtist(std::string_view val)
  {
    _albumArtist = val;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::composer(std::string_view val)
  {
    _composer = val;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::genre(std::string_view val)
  {
    _genre = val;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::work(std::string_view val)
  {
    _work = val;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::year(std::uint16_t val)
  {
    _year = val;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::trackNumber(std::uint16_t val)
  {
    _trackNumber = val;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::totalTracks(std::uint16_t val)
  {
    _totalTracks = val;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::discNumber(std::uint16_t val)
  {
    _discNumber = val;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::totalDiscs(std::uint16_t val)
  {
    _totalDiscs = val;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::coverArtId(std::uint32_t val)
  {
    _coverArtId = val;
    _embeddedCoverArt = {};
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::coverArtData(std::span<std::byte const> val)
  {
    _embeddedCoverArt = val;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::rating(std::uint8_t val)
  {
    _rating = val;
    return *this;
  }

  //=============================================================================
  // PropertyBuilder
  //=============================================================================

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::fileSize(std::uint64_t val)
  {
    _fileSize = val;
    return *this;
  }

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::mtime(std::uint64_t val)
  {
    _mtime = val;
    return *this;
  }

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::durationMs(std::uint32_t val)
  {
    _durationMs = val;
    return *this;
  }

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::bitrate(std::uint32_t val)
  {
    _bitrate = val;
    return *this;
  }

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::sampleRate(std::uint32_t val)
  {
    _sampleRate = val;
    return *this;
  }

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::codecId(std::uint16_t val)
  {
    _codecId = val;
    return *this;
  }

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::channels(std::uint8_t val)
  {
    _channels = val;
    return *this;
  }

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::bitDepth(std::uint8_t val)
  {
    _bitDepth = val;
    return *this;
  }

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::uri(std::string_view val)
  {
    _uri = val;
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

    if (!builder->_metadataBuilder._composer.empty())
    {
      _composerId = dict.put(txn, builder->_metadataBuilder._composer);
    }

    _bloomFilter = computeBloomFilter(_tagIds);

    // Compute total hot size: Header(36) + tags + title
    _size = sizeof(TrackHotHeader);
    _size += _tagIds.size() * sizeof(DictionaryId);
    _size += builder->_metadataBuilder._title.size();
    _size = (_size + 3) & ~3; // NOLINT(readability-magic-numbers) pad to 4 bytes
  }

  void TrackBuilder::PreparedHot::writeTo(std::span<std::byte> out) const
  {
    // Exact size validation and alignment check
    gsl_Expects(out.size() == _size);
    gsl_Expects((std::uintptr_t(out.data()) % 4) == 0);

    auto const& builder = *_builder;
    new (out.data()) TrackHotHeader{
      .tagBloom = _bloomFilter,
      .artistId = _artistId,
      .albumId = _albumId,
      .genreId = _genreId,
      .albumArtistId = _albumArtistId,
      .composerId = _composerId,
      .year = builder._metadataBuilder._year,
      .codecId = builder._propertyBuilder._codecId,
      .bitDepth = builder._propertyBuilder._bitDepth,
      .titleLen = static_cast<std::uint16_t>(builder._metadataBuilder._title.size()),
      .tagLen = static_cast<std::uint16_t>(_tagIds.size() * sizeof(DictionaryId)),
      .rating = builder._metadataBuilder._rating,
      .padding = std::byte{0},
    };

    auto pos = sizeof(TrackHotHeader);

    if (!_tagIds.empty())
    {
      auto const tagBytes = utility::bytes::view(std::span<DictionaryId const>{_tagIds});
      std::memcpy(out.data() + pos, tagBytes.data(), tagBytes.size());
      pos += tagBytes.size();
    }

    // Write title

    if (!builder._metadataBuilder._title.empty())
    {
      std::memcpy(out.data() + pos, builder._metadataBuilder._title.data(), builder._metadataBuilder._title.size());
      pos += builder._metadataBuilder._title.size();
    }

    // Pad to 4 bytes

    while (pos % 4 != 0)
    {
      out[pos++] = std::byte{0};
    }
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

    if (!_builder->_metadataBuilder._work.empty())
    {
      _workId = dict.put(txn, _builder->_metadataBuilder._work);
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

    // NOLINTNEXTLINE(readability-identifier-length)
    for (auto const& [_, value] : _resolvedPairs)
    {
      totalValueSize += value.size();
    }

    _uriLen = static_cast<std::uint16_t>(_builder->_propertyBuilder._uri.size());

    // Cold layout: header(52) + entries(N*8) + values + uri
    std::size_t size = sizeof(TrackColdHeader);
    constexpr std::size_t kEntrySize = 8;
    constexpr std::size_t kAlignmentBytes = 4;
    constexpr std::size_t kAlignmentMask = kAlignmentBytes - 1;
    size += entryCount * kEntrySize;
    size += totalValueSize;
    size += _uriLen;
    size = (size + kAlignmentMask) & ~kAlignmentMask;

    _uriOffset = static_cast<std::uint16_t>(sizeof(TrackColdHeader) + (entryCount * kEntrySize) + totalValueSize);
    _size = size;
  }

  void TrackBuilder::PreparedCold::writeTo(std::span<std::byte> out) const
  {
    // Exact size validation and alignment check
    gsl_Expects(out.size() == _size);
    gsl_Expects((std::uintptr_t(out.data()) % 4) == 0);

    auto const& meta = _builder->_metadataBuilder;
    auto const& prop = _builder->_propertyBuilder;

    {
      auto [fileSizeLo, fileSizeHi] = utility::uint64Parts::split(prop._fileSize);
      auto [mtimeLo, mtimeHi] = utility::uint64Parts::split(prop._mtime);
      new (out.data()) TrackColdHeader{
        .fileSizeLo = fileSizeLo,
        .fileSizeHi = fileSizeHi,
        .mtimeLo = mtimeLo,
        .mtimeHi = mtimeHi,
        .durationMs = prop._durationMs,
        .sampleRate = prop._sampleRate,
        .coverArtId = _coverArtId,
        .bitrate = prop._bitrate,
        .workId = _workId,
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

    auto pos = sizeof(TrackColdHeader);

    // Write entries: dictId(4) + offset(2) + len(2) each, all 4-byte aligned
    struct Entry
    {
      DictionaryId dictId;
      std::uint16_t offset = 0;
      std::uint16_t len = 0;
    };

    auto valueOffset = std::size_t{sizeof(TrackColdHeader) + (_resolvedPairs.size() * 8)};

    for (auto const& [dictId, value] : _resolvedPairs)
    {
      new (out.data() + pos) Entry{.dictId = dictId,
                                   .offset = static_cast<std::uint16_t>(valueOffset),
                                   .len = static_cast<std::uint16_t>(value.size())};
      pos += sizeof(Entry);
      valueOffset += value.size();
    }

    // Write all values contiguously
    // NOLINTNEXTLINE(readability-identifier-length)
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
      pos += _uriLen;
    }

    // Pad to 4 bytes

    while (pos % 4 != 0)
    {
      out[pos++] = std::byte{0};
    }
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
} // namespace rs::library
