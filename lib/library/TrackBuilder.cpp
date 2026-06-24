// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/Type.h>
#include <ao/library/CoverArt.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackView.h>
#include <ao/library/detail/LibraryError.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/ByteView.h>

#include <gsl-lite/gsl-lite.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <limits>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace ao::library
{
  constexpr std::uint32_t kBloomBitMask = 31;
  constexpr std::size_t kSerializedAlignmentBytes = 4;
  constexpr std::size_t kSerializedAlignmentMask = kSerializedAlignmentBytes - 1;

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

      auto prop = view.property();
      builder.property().sampleRate(prop.sampleRate()).codec(prop.codec()).bitDepth(prop.bitDepth());

      if (auto artistId = meta.artistId(); artistId.raw() > 0)
      {
        builder.metadata().artist(dict.get(artistId));
      }

      if (auto albumId = meta.albumId(); albumId.raw() > 0)
      {
        builder.metadata().album(dict.get(albumId));
      }

      if (auto albumArtistId = meta.albumArtistId(); albumArtistId.raw() > 0)
      {
        builder.metadata().albumArtist(dict.get(albumArtistId));
      }

      if (auto composerId = meta.composerId(); composerId.raw() > 0)
      {
        builder.metadata().composer(dict.get(composerId));
      }

      if (auto genreId = meta.genreId(); genreId.raw() > 0)
      {
        builder.metadata().genre(dict.get(genreId));
      }

      for (auto const tagId : view.tags())
      {
        builder.tags().add(dict.get(tagId));
      }
    }

    if (view.isColdValid())
    {
      auto prop = view.property();
      builder.property().uri(prop.uri()).duration(prop.duration()).bitrate(prop.bitrate()).channels(prop.channels());

      auto meta = view.metadata();
      builder.metadata()
        .trackNumber(meta.trackNumber())
        .trackTotal(meta.trackTotal())
        .discNumber(meta.discNumber())
        .discTotal(meta.discTotal())
        .movementNumber(meta.movementNumber())
        .movementTotal(meta.movementTotal());

      for (auto const cover : view.coverArt())
      {
        builder.coverArt().add(cover.type, cover.resourceId);
      }

      if (auto workId = meta.workId(); workId.raw() > 0)
      {
        builder.metadata().work(dict.get(workId));
      }

      if (auto movementId = meta.movementId(); movementId.raw() > 0)
      {
        builder.metadata().movement(dict.get(movementId));
      }

      for (auto const& [dictId, value] : view.customMetadata())
      {
        builder.customMetadata().add(dict.get(dictId), value);
      }
    }

    return builder;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::metadata()
  {
    return _metadataBuilder;
  }

  TrackBuilder::MetadataBuilder const& TrackBuilder::metadata() const
  {
    return _metadataBuilder;
  }

  TrackBuilder::PropertyBuilder& TrackBuilder::property()
  {
    return _propertyBuilder;
  }

  TrackBuilder::PropertyBuilder const& TrackBuilder::property() const
  {
    return _propertyBuilder;
  }

  TrackBuilder::TagsBuilder& TrackBuilder::tags()
  {
    return _tagsBuilder;
  }

  TrackBuilder::TagsBuilder const& TrackBuilder::tags() const
  {
    return _tagsBuilder;
  }

  TrackBuilder::CoverArtBuilder& TrackBuilder::coverArt()
  {
    return _coverArtBuilder;
  }

  TrackBuilder::CoverArtBuilder const& TrackBuilder::coverArt() const
  {
    return _coverArtBuilder;
  }

  TrackBuilder::CustomMetadataBuilder& TrackBuilder::customMetadata()
  {
    return _customMetadataBuilder;
  }

  TrackBuilder::CustomMetadataBuilder const& TrackBuilder::customMetadata() const
  {
    return _customMetadataBuilder;
  }

  //=============================================================================
  // MetadataBuilder
  //=============================================================================

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::title(std::string_view text)
  {
    _title = text;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::artist(std::string_view text)
  {
    _artist = text;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::album(std::string_view text)
  {
    _album = text;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::albumArtist(std::string_view text)
  {
    _albumArtist = text;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::composer(std::string_view text)
  {
    _composer = text;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::genre(std::string_view text)
  {
    _genre = text;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::work(std::string_view text)
  {
    _work = text;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::movement(std::string_view text)
  {
    _movement = text;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::year(std::uint16_t year)
  {
    _year = year;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::trackNumber(std::uint16_t number)
  {
    _trackNumber = number;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::trackTotal(std::uint16_t count)
  {
    _trackTotal = count;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::discNumber(std::uint16_t number)
  {
    _discNumber = number;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::discTotal(std::uint16_t count)
  {
    _discTotal = count;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::movementNumber(std::uint16_t number)
  {
    _movementNumber = number;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::movementTotal(std::uint16_t count)
  {
    _movementTotal = count;
    return *this;
  }

  //=============================================================================
  // PropertyBuilder
  //=============================================================================

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::duration(std::chrono::milliseconds duration)
  {
    _duration = duration;
    return *this;
  }

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::bitrate(Bitrate bitrate)
  {
    _bitrate = bitrate;
    return *this;
  }

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::sampleRate(SampleRate sampleRate)
  {
    _sampleRate = sampleRate;
    return *this;
  }

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::codec(AudioCodec codec)
  {
    _codec = codec;
    return *this;
  }

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::channels(Channels channels)
  {
    _channels = channels;
    return *this;
  }

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::bitDepth(BitDepth bitDepth)
  {
    _bitDepth = bitDepth;
    return *this;
  }

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::uri(std::string_view uri)
  {
    _uri = uri;
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
  // CoverArtBuilder
  //=============================================================================

  TrackBuilder::CoverArtBuilder& TrackBuilder::CoverArtBuilder::add(PictureType type, ResourceId resourceId)
  {
    if (resourceId != kInvalidResourceId)
    {
      _entries.push_back({.type = type, .source = resourceId});
    }

    return *this;
  }

  TrackBuilder::CoverArtBuilder& TrackBuilder::CoverArtBuilder::add(PictureType type, std::span<std::byte const> data)
  {
    if (!data.empty())
    {
      _entries.push_back({.type = type, .source = data});
    }

    return *this;
  }

  TrackBuilder::CoverArtBuilder& TrackBuilder::CoverArtBuilder::erase(std::size_t index)
  {
    gsl_Expects(index < _entries.size());
    _entries.erase(_entries.begin() + static_cast<std::ptrdiff_t>(index));
    return *this;
  }

  TrackBuilder::CoverArtBuilder& TrackBuilder::CoverArtBuilder::clear()
  {
    _entries.clear();
    return *this;
  }

  //=============================================================================
  // CustomMetadataBuilder
  //=============================================================================

  TrackBuilder::CustomMetadataBuilder& TrackBuilder::CustomMetadataBuilder::add(std::string_view key,
                                                                                std::string_view value)
  {
    _customPairs.emplace_back(key, value);
    return *this;
  }

  TrackBuilder::CustomMetadataBuilder& TrackBuilder::CustomMetadataBuilder::remove(std::string_view key)
  {
    std::erase_if(_customPairs, [&key](auto const& pair) { return pair.first == key; });
    return *this;
  }

  TrackBuilder::CustomMetadataBuilder& TrackBuilder::CustomMetadataBuilder::clear()
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

    for (auto const tagId : tagIds)
    {
      bloom |= (std::uint32_t{1} << (tagId.raw() & kBloomBitMask));
    }

    return bloom;
  }

  Result<std::vector<std::byte>> TrackBuilder::serializeHot(lmdb::WriteTransaction& txn, DictionaryStore& dict) const
  {
    try
    {
      auto const prepared = PreparedHot::create(this, txn, dict);
      auto result = std::vector<std::byte>(prepared.size());

      prepared.writeTo(result);
      return result;
    }
    catch (detail::LibraryException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }

  Result<std::vector<std::byte>> TrackBuilder::serializeCold(lmdb::WriteTransaction& txn,
                                                             DictionaryStore& dict,
                                                             ResourceStore& resources) const
  {
    try
    {
      auto const prepared = PreparedCold::create(this, txn, dict, resources);
      auto result = std::vector<std::byte>(prepared.size());

      prepared.writeTo(result);
      return result;
    }
    catch (detail::LibraryException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }

  Result<std::pair<std::vector<std::byte>, std::vector<std::byte>>>
  TrackBuilder::serialize(lmdb::WriteTransaction& txn, DictionaryStore& dict, ResourceStore& resources) const
  {
    try
    {
      auto const hot = PreparedHot::create(this, txn, dict);
      auto const cold = PreparedCold::create(this, txn, dict, resources);

      auto hotBytes = std::vector<std::byte>(hot.size());
      auto coldBytes = std::vector<std::byte>(cold.size());

      hot.writeTo(hotBytes);
      cold.writeTo(coldBytes);
      return std::pair{std::move(hotBytes), std::move(coldBytes)};
    }
    catch (detail::LibraryException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }

  //=============================================================================
  // Prepared structures for zero-copy serialization
  //=============================================================================

  TrackBuilder::PreparedHot::PreparedHot(TrackBuilder const* builder)
    : _builder{builder}
  {
  }

  TrackBuilder::PreparedHot TrackBuilder::PreparedHot::create(TrackBuilder const* builder,
                                                              lmdb::WriteTransaction& txn,
                                                              DictionaryStore& dict)
  {
    auto prepared = PreparedHot{builder};

    // Resolve tag names to DictionaryIds
    prepared._tagIds.reserve(builder->_tagsBuilder._tagNames.size());

    for (auto const& name : builder->_tagsBuilder._tagNames)
    {
      auto idResult = dict.put(txn, name);

      if (!idResult)
      {
        detail::throwLibraryError(idResult.error());
      }

      prepared._tagIds.push_back(*idResult);
    }

    // Resolve metadata strings to DictionaryIds for header
    if (!builder->_metadataBuilder._artist.empty())
    {
      auto idResult = dict.put(txn, builder->_metadataBuilder._artist);

      if (!idResult)
      {
        detail::throwLibraryError(idResult.error());
      }

      prepared._artistId = *idResult;
    }

    if (!builder->_metadataBuilder._album.empty())
    {
      auto idResult = dict.put(txn, builder->_metadataBuilder._album);

      if (!idResult)
      {
        detail::throwLibraryError(idResult.error());
      }

      prepared._albumId = *idResult;
    }

    if (!builder->_metadataBuilder._genre.empty())
    {
      auto idResult = dict.put(txn, builder->_metadataBuilder._genre);

      if (!idResult)
      {
        detail::throwLibraryError(idResult.error());
      }

      prepared._genreId = *idResult;
    }

    if (!builder->_metadataBuilder._albumArtist.empty())
    {
      auto idResult = dict.put(txn, builder->_metadataBuilder._albumArtist);

      if (!idResult)
      {
        detail::throwLibraryError(idResult.error());
      }

      prepared._albumArtistId = *idResult;
    }

    if (!builder->_metadataBuilder._composer.empty())
    {
      auto idResult = dict.put(txn, builder->_metadataBuilder._composer);

      if (!idResult)
      {
        detail::throwLibraryError(idResult.error());
      }

      prepared._composerId = *idResult;
    }

    prepared._bloomFilter = computeBloomFilter(prepared._tagIds);

    // Compute total hot size: Header(36) + tags + title
    prepared._size = sizeof(TrackHotHeader);
    prepared._size += prepared._tagIds.size() * sizeof(DictionaryId);
    prepared._size += builder->_metadataBuilder._title.size();
    prepared._size = (prepared._size + kSerializedAlignmentMask) & ~kSerializedAlignmentMask;
    return prepared;
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
      .sampleRate = builder._propertyBuilder._sampleRate,
      .year = builder._metadataBuilder._year,
      .titleLength = static_cast<std::uint16_t>(builder._metadataBuilder._title.size()),
      .tagLength = static_cast<std::uint16_t>(_tagIds.size() * sizeof(DictionaryId)),
      .bitDepth = builder._propertyBuilder._bitDepth,
      .codec = builder._propertyBuilder._codec,
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

  TrackBuilder::PreparedCold::PreparedCold(TrackBuilder const* builder)
    : _builder{builder}
  {
  }

  TrackBuilder::PreparedCold TrackBuilder::PreparedCold::create(TrackBuilder const* builder,
                                                                lmdb::WriteTransaction& txn,
                                                                DictionaryStore& dict,
                                                                ResourceStore& resources)
  {
    auto prepared = PreparedCold{builder};

    if (!prepared._builder->_metadataBuilder._work.empty())
    {
      auto idResult = dict.put(txn, prepared._builder->_metadataBuilder._work);

      if (!idResult)
      {
        detail::throwLibraryError(idResult.error());
      }

      prepared._workId = *idResult;
    }

    if (!prepared._builder->_metadataBuilder._movement.empty())
    {
      auto idResult = dict.put(txn, prepared._builder->_metadataBuilder._movement);

      if (!idResult)
      {
        detail::throwLibraryError(idResult.error());
      }

      prepared._movementId = *idResult;
    }

    // Resolve custom keys to DictionaryIds
    prepared._resolvedPairs.reserve(prepared._builder->_customMetadataBuilder._customPairs.size());

    for (auto const& [key, value] : prepared._builder->_customMetadataBuilder._customPairs)
    {
      auto idResult = dict.put(txn, key);

      if (!idResult)
      {
        detail::throwLibraryError(idResult.error());
      }

      prepared._resolvedPairs.emplace_back(*idResult, value);
    }

    // Sort by dictId for binary search
    std::ranges::sort(prepared._resolvedPairs, {}, &std::pair<DictionaryId, std::string_view>::first);

    // Resolve pending cover blobs into ResourceStore
    {
      auto writer = resources.writer(txn);
      prepared._coverArt.reserve(prepared._builder->_coverArtBuilder._entries.size());

      for (auto const& pending : prepared._builder->_coverArtBuilder._entries)
      {
        if (auto const* ptrResourceId = std::get_if<ResourceId>(&pending.source); ptrResourceId != nullptr)
        {
          prepared._coverArt.push_back({.resourceId = *ptrResourceId, .type = pending.type});
        }
        else
        {
          auto const data = std::get<std::span<std::byte const>>(pending.source);
          auto resourceResult = writer.create(data);

          if (!resourceResult)
          {
            detail::throwLibraryError(resourceResult.error());
          }

          prepared._coverArt.push_back({.resourceId = *resourceResult, .type = pending.type});
        }
      }
    }

    // Compute sizes with overflow validation.
    // All offset/count/length fields in TrackColdHeader are uint16_t, so every
    // intermediate value must be checked before narrowing.
    constexpr std::size_t kU16Max = std::numeric_limits<std::uint16_t>::max();

    std::size_t const coverCount = prepared._coverArt.size();
    std::size_t const entryCount = prepared._resolvedPairs.size();
    std::size_t totalValueSize = 0;

    for (auto const& resolvedPair : prepared._resolvedPairs)
    {
      auto const len = resolvedPair.second.size();

      if (len > kU16Max)
      {
        detail::throwLibraryError(
          Error::Code::ValueTooLarge, std::format("Custom metadata value length {} exceeds uint16_t", len));
      }

      totalValueSize += len;
    }

    auto const uriSize = prepared._builder->_propertyBuilder._uri.size();

    if (uriSize > kU16Max)
    {
      detail::throwLibraryError(Error::Code::ValueTooLarge, std::format("URI length {} exceeds uint16_t", uriSize));
    }

    if (coverCount > kU16Max)
    {
      detail::throwLibraryError(
        Error::Code::ValueTooLarge, std::format("Cover art count {} exceeds uint16_t", coverCount));
    }

    if (entryCount > kU16Max)
    {
      detail::throwLibraryError(
        Error::Code::ValueTooLarge, std::format("Custom metadata count {} exceeds uint16_t", entryCount));
    }

    prepared._uriLength = static_cast<std::uint16_t>(uriSize);

    constexpr std::size_t kEntrySize = 8;
    std::size_t const coverArea = (coverCount * kEntrySize);
    std::size_t const customOffsetValue = sizeof(TrackColdHeader) + coverArea;

    if (customOffsetValue > kU16Max)
    {
      detail::throwLibraryError(
        Error::Code::ValueTooLarge, std::format("Cold record custom offset {} exceeds uint16_t", customOffsetValue));
    }

    prepared._customOffset = static_cast<std::uint16_t>(customOffsetValue);

    std::size_t const customArea = (entryCount * kEntrySize) + totalValueSize;
    std::size_t const uriOffsetValue = customOffsetValue + customArea;

    if (uriOffsetValue > kU16Max)
    {
      detail::throwLibraryError(
        Error::Code::ValueTooLarge, std::format("Cold record URI offset {} exceeds uint16_t", uriOffsetValue));
    }

    prepared._uriOffset = static_cast<std::uint16_t>(uriOffsetValue);

    std::size_t size = uriOffsetValue + uriSize;
    size = (size + kSerializedAlignmentMask) & ~kSerializedAlignmentMask;
    prepared._size = size;

    return prepared;
  }

  void TrackBuilder::PreparedCold::writeTo(std::span<std::byte> out) const
  {
    // Exact size validation and alignment check
    gsl_Expects(out.size() == _size);
    gsl_Expects((std::uintptr_t(out.data()) % 4) == 0);

    auto const& meta = _builder->_metadataBuilder;
    auto const& prop = _builder->_propertyBuilder;

    {
      new (out.data()) TrackColdHeader{
        .duration = std::chrono::duration_cast<TrackDuration>(prop._duration),
        .bitrate = prop._bitrate,
        .workId = _workId,
        .movementId = _movementId,
        .trackNumber = meta._trackNumber,
        .trackTotal = meta._trackTotal,
        .discNumber = meta._discNumber,
        .discTotal = meta._discTotal,
        .movementNumber = meta._movementNumber,
        .movementTotal = meta._movementTotal,
        .customCount = static_cast<std::uint16_t>(_resolvedPairs.size()), // validated in prepare
        .uriOffset = _uriOffset,
        .uriLength = _uriLength,
        .coverCount = static_cast<std::uint16_t>(_coverArt.size()), // validated in prepare
        .customOffset = _customOffset,
        .channels = prop._channels,
        .padding = {},
      };
    }

    auto pos = sizeof(TrackColdHeader);

    // Write cover table immediately after header: resourceId(4) + type(1) + reserved(3) each
    for (auto const& cover : _coverArt)
    {
      new (out.data() + pos) CoverArtEntry{.id = cover.resourceId, .type = static_cast<std::uint8_t>(cover.type)};
      pos += sizeof(CoverArtEntry);
    }

    gsl_Assert(pos == _customOffset);

    std::size_t valueOffset = _customOffset + (_resolvedPairs.size() * sizeof(CustomMetadataEntry));

    for (auto const& [dictId, value] : _resolvedPairs)
    {
      new (out.data() + pos) CustomMetadataEntry{.keyId = dictId,
                                                 .valueOffset = static_cast<std::uint16_t>(valueOffset),
                                                 .valueLength = static_cast<std::uint16_t>(value.size())};
      pos += sizeof(CustomMetadataEntry);
      valueOffset += value.size();
    }

    // Write all custom values contiguously
    for (auto const& resolvedPair : _resolvedPairs)
    {
      auto const& value = resolvedPair.second;

      if (!value.empty())
      {
        std::memcpy(out.data() + pos, value.data(), value.size());
      }

      pos += value.size();
    }

    gsl_Assert(pos == _uriOffset);

    // Write uri
    if (_uriLength > 0)
    {
      std::memcpy(out.data() + pos, _builder->_propertyBuilder._uri.data(), _uriLength);
      pos += _uriLength;
    }

    // Pad to 4 bytes
    while (pos % 4 != 0)
    {
      out[pos++] = std::byte{0};
    }
  }

  Result<std::pair<TrackBuilder::PreparedHot, TrackBuilder::PreparedCold>>
  TrackBuilder::prepare(lmdb::WriteTransaction& txn, DictionaryStore& dict, ResourceStore& resources) const
  {
    try
    {
      return std::pair{PreparedHot::create(this, txn, dict), PreparedCold::create(this, txn, dict, resources)};
    }
    catch (detail::LibraryException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }

  Result<TrackBuilder::PreparedHot> TrackBuilder::prepareHot(lmdb::WriteTransaction& txn, DictionaryStore& dict) const
  {
    try
    {
      return PreparedHot::create(this, txn, dict);
    }
    catch (detail::LibraryException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }

  Result<TrackBuilder::PreparedCold> TrackBuilder::prepareCold(lmdb::WriteTransaction& txn,
                                                               DictionaryStore& dict,
                                                               ResourceStore& resources) const
  {
    try
    {
      return PreparedCold::create(this, txn, dict, resources);
    }
    catch (detail::LibraryException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }
} // namespace ao::library
