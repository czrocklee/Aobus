// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
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
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ao::library
{
  namespace
  {
    constexpr std::uint32_t kBloomBitMask = 31;
    constexpr std::size_t kSerializedAlignmentBytes = 4;
    constexpr std::size_t kSerializedAlignmentMask = kSerializedAlignmentBytes - 1;

    constexpr std::size_t align4(std::size_t size) noexcept
    {
      return (size + kSerializedAlignmentMask) & ~kSerializedAlignmentMask;
    }

    constexpr std::size_t kU16Max = std::numeric_limits<std::uint16_t>::max();

    std::uint16_t checkedUint16(std::size_t value, std::string_view label)
    {
      if (value > kU16Max)
      {
        detail::throwLibraryError(Error::Code::ValueTooLarge, std::format("{} {} exceeds uint16_t", label, value));
      }

      return static_cast<std::uint16_t>(value);
    }

    std::size_t checkedPayloadBytes(std::size_t count, std::size_t elementSize, std::string_view label)
    {
      if (count > std::numeric_limits<std::uint16_t>::max() / elementSize)
      {
        detail::throwLibraryError(Error::Code::ValueTooLarge, std::format("{} exceeds uint16_t", label));
      }

      auto const byteCount = count * elementSize;
      checkedUint16(byteCount, label);
      return byteCount;
    }

    DictionaryId resolveDictionaryId(std::string_view value,
                                     lmdb::WriteTransaction& transaction,
                                     DictionaryStore& dictionary)
    {
      if (value.empty())
      {
        return kInvalidDictionaryId;
      }

      auto idResult = dictionary.put(transaction, value);

      if (!idResult)
      {
        detail::throwLibraryError(idResult.error());
      }

      return *idResult;
    }

    template<typename T>
    void writePod(std::span<std::byte> out, std::size_t offset, T const& value)
    {
      static_assert(std::is_trivially_copyable_v<T>);
      static_assert(std::is_standard_layout_v<T>);
      static_assert(alignof(T) <= kSerializedAlignmentBytes);

      gsl_Expects((offset % alignof(T)) == 0);
      gsl_Expects(offset + sizeof(T) <= out.size());

      std::memcpy(out.data() + offset, &value, sizeof(T));
    }
  } // namespace

  //=============================================================================
  // TrackBuilder - factory methods
  //=============================================================================

  TrackBuilder TrackBuilder::makeEmpty()
  {
    return TrackBuilder{};
  }

  TrackBuilder TrackBuilder::fromView(TrackView const& view, DictionaryStore& dictionary)
  {
    auto builder = TrackBuilder{};

    if (view.isHotValid())
    {
      auto metadata = view.metadata();
      builder.metadata().title(metadata.title()).year(metadata.year());

      auto prop = view.property();
      builder.property().sampleRate(prop.sampleRate()).codec(prop.codec()).bitDepth(prop.bitDepth());

      if (auto artistId = metadata.artistId(); artistId.raw() > 0)
      {
        builder.metadata().artist(dictionary.get(artistId));
      }

      if (auto albumId = metadata.albumId(); albumId.raw() > 0)
      {
        builder.metadata().album(dictionary.get(albumId));
      }

      if (auto albumArtistId = metadata.albumArtistId(); albumArtistId.raw() > 0)
      {
        builder.metadata().albumArtist(dictionary.get(albumArtistId));
      }

      if (auto composerId = metadata.composerId(); composerId.raw() > 0)
      {
        builder.metadata().composer(dictionary.get(composerId));
      }

      if (auto genreId = metadata.genreId(); genreId.raw() > 0)
      {
        builder.metadata().genre(dictionary.get(genreId));
      }

      for (auto const tagId : view.tags())
      {
        builder.tags().add(dictionary.get(tagId));
      }
    }

    if (view.isColdValid())
    {
      auto prop = view.property();
      builder.property().uri(prop.uri()).duration(prop.duration()).bitrate(prop.bitrate()).channels(prop.channels());

      auto metadata = view.metadata();
      builder.metadata()
        .trackNumber(metadata.trackNumber())
        .trackTotal(metadata.trackTotal())
        .discNumber(metadata.discNumber())
        .discTotal(metadata.discTotal());

      auto classical = view.classical();
      builder.metadata().movementNumber(classical.movementNumber()).movementTotal(classical.movementTotal());

      for (auto const cover : view.coverArt())
      {
        builder.coverArt().add(cover.type, cover.resourceId);
      }

      if (auto workId = classical.workId(); workId.raw() > 0)
      {
        builder.metadata().work(dictionary.get(workId));
      }

      if (auto movementId = classical.movementId(); movementId.raw() > 0)
      {
        builder.metadata().movement(dictionary.get(movementId));
      }

      if (auto conductorId = classical.conductorId(); conductorId.raw() > 0)
      {
        builder.metadata().conductor(dictionary.get(conductorId));
      }

      if (auto ensembleId = classical.ensembleId(); ensembleId.raw() > 0)
      {
        builder.metadata().ensemble(dictionary.get(ensembleId));
      }

      if (auto soloistId = classical.soloistId(); soloistId.raw() > 0)
      {
        builder.metadata().soloist(dictionary.get(soloistId));
      }

      for (auto const& [dictionaryId, value] : view.customMetadata())
      {
        builder.customMetadata().add(dictionary.get(dictionaryId), value);
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

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::conductor(std::string_view text)
  {
    _conductor = text;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::ensemble(std::string_view text)
  {
    _ensemble = text;
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

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::soloist(std::string_view text)
  {
    _soloist = text;
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

  Result<std::vector<std::byte>> TrackBuilder::serializeHot(lmdb::WriteTransaction& transaction,
                                                            DictionaryStore& dictionary) const
  {
    try
    {
      auto const prepared = PreparedHot::create(this, transaction, dictionary);
      auto result = std::vector<std::byte>(prepared.size());

      prepared.writeTo(result);
      return result;
    }
    catch (detail::LibraryException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }

  Result<std::vector<std::byte>> TrackBuilder::serializeCold(lmdb::WriteTransaction& transaction,
                                                             DictionaryStore& dictionary,
                                                             ResourceStore& resources) const
  {
    try
    {
      auto const prepared = PreparedCold::create(this, transaction, dictionary, resources);
      auto result = std::vector<std::byte>(prepared.size());

      prepared.writeTo(result);
      return result;
    }
    catch (detail::LibraryException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }

  Result<std::pair<std::vector<std::byte>, std::vector<std::byte>>> TrackBuilder::serialize(
    lmdb::WriteTransaction& transaction,
    DictionaryStore& dictionary,
    ResourceStore& resources) const
  {
    try
    {
      auto const hot = PreparedHot::create(this, transaction, dictionary);
      auto const cold = PreparedCold::create(this, transaction, dictionary, resources);

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

  TrackBuilder::PreparedHot TrackBuilder::PreparedHot::create(TrackBuilder const* builder,
                                                              lmdb::WriteTransaction& transaction,
                                                              DictionaryStore& dictionary)
  {
    auto prepared = PreparedHot{};

    // Resolve tag names to DictionaryIds
    prepared._tagIds.reserve(builder->_tagsBuilder._tagNames.size());

    for (auto const& name : builder->_tagsBuilder._tagNames)
    {
      auto idResult = dictionary.put(transaction, name);

      if (!idResult)
      {
        detail::throwLibraryError(idResult.error());
      }

      prepared._tagIds.push_back(*idResult);
    }

    // Resolve metadata strings to DictionaryIds for header
    auto const& metadata = builder->_metadataBuilder;
    prepared._artistId = resolveDictionaryId(metadata._artist, transaction, dictionary);
    prepared._albumId = resolveDictionaryId(metadata._album, transaction, dictionary);
    prepared._genreId = resolveDictionaryId(metadata._genre, transaction, dictionary);
    prepared._albumArtistId = resolveDictionaryId(metadata._albumArtist, transaction, dictionary);
    prepared._composerId = resolveDictionaryId(metadata._composer, transaction, dictionary);

    prepared._bloomFilter = computeBloomFilter(prepared._tagIds);

    // Hot header lengths are uint16_t. Reject anything the header cannot
    // represent so writeTo's narrowing casts stay lossless and the write
    // side remains the single validation boundary for readers.
    auto const titleLength = builder->_metadataBuilder._title.size();
    auto const tagLength = prepared._tagIds.size() * sizeof(DictionaryId);

    // Snapshot everything writeTo emits so the prepared value stays valid
    // and self-consistent even if the builder is mutated or destroyed later.
    prepared._title = std::string{builder->_metadataBuilder._title};
    prepared._titleLength = checkedUint16(titleLength, "Hot title length");
    prepared._tagLength = checkedUint16(tagLength, "Hot tag payload length");
    prepared._sampleRate = builder->_propertyBuilder._sampleRate;
    prepared._year = builder->_metadataBuilder._year;
    prepared._bitDepth = builder->_propertyBuilder._bitDepth;
    prepared._codec = builder->_propertyBuilder._codec;

    // Compute total hot size: header + tags + title
    prepared._size = sizeof(TrackHotHeader);
    prepared._size += tagLength;
    prepared._size += titleLength;
    prepared._size = (prepared._size + kSerializedAlignmentMask) & ~kSerializedAlignmentMask;
    return prepared;
  }

  void TrackBuilder::PreparedHot::writeTo(std::span<std::byte> out) const
  {
    // Exact size validation and alignment check
    gsl_Expects(out.size() == _size);
    gsl_Expects(utility::bytes::isAligned(out.data(), kSerializedAlignmentBytes));

    writePod(out,
             0,
             TrackHotHeader{
               .tagBloom = _bloomFilter,
               .artistId = _artistId,
               .albumId = _albumId,
               .genreId = _genreId,
               .albumArtistId = _albumArtistId,
               .composerId = _composerId,
               .sampleRate = _sampleRate,
               .year = _year,
               .titleLength = _titleLength,
               .tagLength = _tagLength,
               .bitDepth = _bitDepth,
               .codec = _codec,
             });

    auto offset = sizeof(TrackHotHeader);

    if (!_tagIds.empty())
    {
      auto const tagBytes = utility::bytes::view(std::span<DictionaryId const>{_tagIds});
      std::memcpy(out.data() + offset, tagBytes.data(), tagBytes.size());
      offset += tagBytes.size();
    }

    // Write title
    if (!_title.empty())
    {
      std::memcpy(out.data() + offset, _title.data(), _title.size());
      offset += _title.size();
    }

    // Pad to 4 bytes
    while (offset % 4 != 0)
    {
      out[offset++] = std::byte{0};
    }
  }

  TrackBuilder::PreparedCold TrackBuilder::PreparedCold::create(TrackBuilder const* builder,
                                                                lmdb::WriteTransaction& transaction,
                                                                DictionaryStore& dictionary,
                                                                ResourceStore& resources)
  {
    auto prepared = PreparedCold{};
    prepared.resolveClassicalIds(builder, transaction, dictionary);
    auto const resolvedPairs = resolveCustomMetadata(builder, transaction, dictionary);
    prepared.resolveCoverArt(builder, transaction, resources);
    prepared.appendCoverArtBlock();
    prepared.appendClassicalBlock(builder->_metadataBuilder);
    prepared.appendCustomMetadataBlock(resolvedPairs);
    prepared.assignLayout(builder->_propertyBuilder._uri);
    prepared.snapshot(builder);
    return prepared;
  }

  void TrackBuilder::PreparedCold::resolveClassicalIds(TrackBuilder const* builder,
                                                       lmdb::WriteTransaction& transaction,
                                                       DictionaryStore& dictionary)
  {
    auto const& metadata = builder->_metadataBuilder;
    _workId = resolveDictionaryId(metadata._work, transaction, dictionary);
    _movementId = resolveDictionaryId(metadata._movement, transaction, dictionary);
    _conductorId = resolveDictionaryId(metadata._conductor, transaction, dictionary);
    _ensembleId = resolveDictionaryId(metadata._ensemble, transaction, dictionary);
    _soloistId = resolveDictionaryId(metadata._soloist, transaction, dictionary);
  }

  std::vector<std::pair<DictionaryId, std::string_view>> TrackBuilder::PreparedCold::resolveCustomMetadata(
    TrackBuilder const* builder,
    lmdb::WriteTransaction& transaction,
    DictionaryStore& dictionary)
  {
    auto resolvedPairs = std::vector<std::pair<DictionaryId, std::string_view>>{};
    resolvedPairs.reserve(builder->_customMetadataBuilder._customPairs.size());

    for (auto const& [key, value] : builder->_customMetadataBuilder._customPairs)
    {
      auto idResult = dictionary.put(transaction, key);

      if (!idResult)
      {
        detail::throwLibraryError(idResult.error());
      }

      resolvedPairs.emplace_back(*idResult, value);
    }

    std::ranges::sort(resolvedPairs, {}, &std::pair<DictionaryId, std::string_view>::first);
    return resolvedPairs;
  }

  void TrackBuilder::PreparedCold::resolveCoverArt(TrackBuilder const* builder,
                                                   lmdb::WriteTransaction& transaction,
                                                   ResourceStore& resources)
  {
    auto writer = resources.writer(transaction);
    _coverArt.reserve(builder->_coverArtBuilder._entries.size());

    for (auto const& pending : builder->_coverArtBuilder._entries)
    {
      if (auto const* ptrResourceId = std::get_if<ResourceId>(&pending.source); ptrResourceId != nullptr)
      {
        _coverArt.push_back({.resourceId = *ptrResourceId, .type = pending.type});
        continue;
      }

      auto const data = std::get<std::span<std::byte const>>(pending.source);
      auto resourceResult = writer.create(data);

      if (!resourceResult)
      {
        detail::throwLibraryError(resourceResult.error());
      }

      _coverArt.push_back({.resourceId = *resourceResult, .type = pending.type});
    }
  }

  void TrackBuilder::PreparedCold::appendBlock(TrackColdBlockSlot slot, std::vector<std::byte> payload)
  {
    checkedUint16(payload.size(), "Cold block payload length");
    _blocks.push_back({.slot = slot, .payload = std::move(payload)});
  }

  void TrackBuilder::PreparedCold::appendCoverArtBlock()
  {
    if (_coverArt.empty())
    {
      return;
    }

    auto const payloadSize = checkedPayloadBytes(_coverArt.size(), sizeof(CoverArtEntry), "Cover art payload length");
    auto payload = std::vector<std::byte>(payloadSize, std::byte{0});
    std::size_t offset = 0;

    for (auto const& cover : _coverArt)
    {
      writePod(std::span<std::byte>{payload},
               offset,
               CoverArtEntry{.id = cover.resourceId, .type = static_cast<std::uint8_t>(cover.type)});
      offset += sizeof(CoverArtEntry);
    }

    appendBlock(TrackColdBlockSlot::CoverArt, std::move(payload));
  }

  void TrackBuilder::PreparedCold::appendClassicalBlock(MetadataBuilder const& metadata)
  {
    bool const hasClassical = _workId != kInvalidDictionaryId || _movementId != kInvalidDictionaryId ||
                              _conductorId != kInvalidDictionaryId || _ensembleId != kInvalidDictionaryId ||
                              _soloistId != kInvalidDictionaryId || metadata._movementNumber != 0 ||
                              metadata._movementTotal != 0;

    if (!hasClassical)
    {
      return;
    }

    auto payload = std::vector<std::byte>(sizeof(TrackClassicalBlock), std::byte{0});
    writePod(std::span<std::byte>{payload},
             0,
             TrackClassicalBlock{
               .workId = _workId,
               .movementId = _movementId,
               .conductorId = _conductorId,
               .ensembleId = _ensembleId,
               .soloistId = _soloistId,
               .movementNumber = metadata._movementNumber,
               .movementTotal = metadata._movementTotal,
             });
    appendBlock(TrackColdBlockSlot::Classical, std::move(payload));
  }

  void TrackBuilder::PreparedCold::appendCustomMetadataBlock(
    std::vector<std::pair<DictionaryId, std::string_view>> const& resolvedPairs)
  {
    if (resolvedPairs.empty())
    {
      return;
    }

    auto const entryCount = resolvedPairs.size();
    auto const entryBytes =
      checkedPayloadBytes(entryCount, sizeof(CustomMetadataEntry), "Custom metadata entry table length");
    auto const valueOffset = sizeof(CustomMetadataBlockHeader) + entryBytes;
    checkedUint16(entryCount, "Custom metadata count");
    checkedUint16(valueOffset, "Custom metadata value offset");

    std::size_t totalValueSize = 0;

    for (auto const& pair : resolvedPairs)
    {
      checkedUint16(pair.second.size(), "Custom metadata value length");

      if (pair.second.size() > kU16Max - totalValueSize)
      {
        detail::throwLibraryError(Error::Code::ValueTooLarge, "Custom metadata payload length exceeds uint16_t");
      }

      totalValueSize += pair.second.size();
    }

    auto const payloadSize = valueOffset + totalValueSize;
    checkedUint16(payloadSize, "Custom metadata payload length");

    auto payload = std::vector<std::byte>(payloadSize, std::byte{0});
    writePod(std::span<std::byte>{payload},
             0,
             CustomMetadataBlockHeader{.entryCount = static_cast<std::uint16_t>(entryCount),
                                       .valueOffset = static_cast<std::uint16_t>(valueOffset),
                                       .payloadLength = static_cast<std::uint16_t>(payloadSize)});

    auto entryOffset = sizeof(CustomMetadataBlockHeader);
    auto valueWriteOffset = valueOffset;

    for (auto const& [dictionaryId, value] : resolvedPairs)
    {
      checkedUint16(valueWriteOffset, "Custom metadata entry value offset");
      writePod(std::span<std::byte>{payload},
               entryOffset,
               CustomMetadataEntry{.keyId = dictionaryId,
                                   .valueOffset = static_cast<std::uint16_t>(valueWriteOffset),
                                   .valueLength = static_cast<std::uint16_t>(value.size())});
      entryOffset += sizeof(CustomMetadataEntry);

      if (!value.empty())
      {
        std::memcpy(payload.data() + valueWriteOffset, value.data(), value.size());
      }

      valueWriteOffset += value.size();
    }

    appendBlock(TrackColdBlockSlot::CustomMetadata, std::move(payload));
  }

  void TrackBuilder::PreparedCold::assignLayout(std::string_view uri)
  {
    std::size_t offset = sizeof(TrackColdHeader);

    for (auto const& block : _blocks)
    {
      auto const slotIndex = trackColdBlockSlotIndex(block.slot);
      gsl_Expects(slotIndex < kTrackColdKnownBlockSlotCount);
      _blockOffsets[slotIndex] = checkedUint16(offset, "Cold block offset");

      auto const rawBlockLength = block.payload.size();
      auto const alignedBlockLength = align4(rawBlockLength);

      if (alignedBlockLength < rawBlockLength || alignedBlockLength > kU16Max - offset)
      {
        detail::throwLibraryError(Error::Code::ValueTooLarge, "Cold block length exceeds uint16_t");
      }

      offset += alignedBlockLength;
    }

    auto const uriSize = uri.size();
    _uriLength = checkedUint16(uriSize, "URI length");
    _uriOffset = checkedUint16(offset, "Cold record URI offset");

    auto const unalignedSize = offset + uriSize;
    auto const recordSize = align4(unalignedSize);

    if (recordSize < unalignedSize || recordSize > kU16Max)
    {
      detail::throwLibraryError(
        Error::Code::ValueTooLarge, std::format("Cold record size {} exceeds uint16_t", recordSize));
    }

    _size = recordSize;
  }

  void TrackBuilder::PreparedCold::snapshot(TrackBuilder const* builder)
  {
    auto const& metadata = builder->_metadataBuilder;
    auto const& property = builder->_propertyBuilder;

    _uri = std::string{property._uri};
    _duration = std::chrono::duration_cast<TrackDuration>(property._duration);
    _bitrate = property._bitrate;
    _trackNumber = metadata._trackNumber;
    _trackTotal = metadata._trackTotal;
    _discNumber = metadata._discNumber;
    _discTotal = metadata._discTotal;
    _channels = property._channels;
  }

  void TrackBuilder::PreparedCold::writeTo(std::span<std::byte> out) const
  {
    // Exact size validation and alignment check
    gsl_Expects(out.size() == _size);
    gsl_Expects(utility::bytes::isAligned(out.data(), kSerializedAlignmentBytes));

    std::ranges::fill(out, std::byte{0});

    writePod(out,
             0,
             TrackColdHeader{
               .duration = _duration,
               .bitrate = _bitrate,
               .trackNumber = _trackNumber,
               .trackTotal = _trackTotal,
               .discNumber = _discNumber,
               .discTotal = _discTotal,
               .blockOffsets = _blockOffsets,
               .uriOffset = _uriOffset,
               .uriLength = _uriLength,
               .channels = _channels,
               .reserved8 = 0,
             });

    std::size_t offset = sizeof(TrackColdHeader);

    for (auto const& block : _blocks)
    {
      gsl_Assert(offset == _blockOffsets[trackColdBlockSlotIndex(block.slot)]);

      if (!block.payload.empty())
      {
        std::memcpy(out.data() + offset, block.payload.data(), block.payload.size());
        offset += block.payload.size();
      }

      offset = align4(offset);
    }

    gsl_Assert(offset == _uriOffset);

    // Write uri
    if (_uriLength > 0)
    {
      std::memcpy(out.data() + offset, _uri.data(), _uriLength);
      offset += _uriLength;
    }

    // Pad to 4 bytes
    while (offset % 4 != 0)
    {
      out[offset++] = std::byte{0};
    }
  }

  Result<std::pair<TrackBuilder::PreparedHot, TrackBuilder::PreparedCold>> TrackBuilder::prepare(
    lmdb::WriteTransaction& transaction,
    DictionaryStore& dictionary,
    ResourceStore& resources) const
  {
    try
    {
      return std::pair{PreparedHot::create(this, transaction, dictionary),
                       PreparedCold::create(this, transaction, dictionary, resources)};
    }
    catch (detail::LibraryException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }

  Result<TrackBuilder::PreparedHot> TrackBuilder::prepareHot(lmdb::WriteTransaction& transaction,
                                                             DictionaryStore& dictionary) const
  {
    try
    {
      return PreparedHot::create(this, transaction, dictionary);
    }
    catch (detail::LibraryException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }

  Result<TrackBuilder::PreparedCold> TrackBuilder::prepareCold(lmdb::WriteTransaction& transaction,
                                                               DictionaryStore& dictionary,
                                                               ResourceStore& resources) const
  {
    try
    {
      return PreparedCold::create(this, transaction, dictionary, resources);
    }
    catch (detail::LibraryException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }
} // namespace ao::library
