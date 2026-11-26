// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/TrackBuilder.h>

#include "CreditAdmission.h"
#include "TextAdmission.h"
#include "TrackPreparation.h"
#include "detail/LibraryError.h"
#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/PictureType.h>
#include <ao/library/Credits.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/LibraryUri.h>
#include <ao/library/RecordingDate.h>
#include <ao/library/ResourceLayout.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackView.h>
#include <ao/library/detail/TrackViewRawAccess.h>
#include <ao/utility/ByteView.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
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

    [[noreturn]] void throwColdRecordTooLarge(std::string_view component)
    {
      detail::throwLibraryError(
        Error::Code::ValueTooLarge,
        std::format(
          "{} would make the padded cold Track record exceed {} bytes", component, kTrackColdRecordSizeLimit));
    }

    // The size of a fixed prefix followed by count entries, when it fits a cold record.
    std::size_t coldTableSize(std::size_t prefixSize,
                              std::size_t count,
                              std::size_t entrySize,
                              std::string_view component)
    {
      if (count > (kTrackColdRecordSizeLimit - prefixSize) / entrySize)
      {
        throwColdRecordTooLarge(component);
      }

      return prefixSize + (count * entrySize);
    }

    Result<> validateColdProperties(TrackBuilder::PropertyBuilder const& property)
    {
      auto uriRes = LibraryUri::parse(property.uri());

      if (!uriRes)
      {
        return std::unexpected{uriRes.error()};
      }

      if (uriRes->value() != property.uri())
      {
        return makeError(Error::Code::InvalidInput, "Track URI is not canonical");
      }

      if (auto const optSegment = property.segment(); optSegment)
      {
        if (optSegment->ordinal == 0 || optSegment->ordinal > kMaxTrackSegmentOrdinal || optSegment->frameCount == 0 ||
            optSegment->startFrame > std::numeric_limits<std::uint64_t>::max() - optSegment->frameCount ||
            !segmentDuration(optSegment->frameCount, property.sampleRate()))
        {
          return makeError(Error::Code::InvalidInput, "Invalid segment range, ordinal, sample rate, or duration");
        }
      }

      if (property.duration() < std::chrono::milliseconds::zero())
      {
        return makeError(Error::Code::InvalidInput, "Track duration must not be negative");
      }

      if (property.duration() > TrackDuration::max())
      {
        return makeError(Error::Code::ValueTooLarge, "Track duration exceeds the signed 32-bit millisecond range");
      }

      return {};
    }

    Result<> validateCreditEntries(std::span<CreditView const> entries)
    {
      // Preflight independently of upstream normalization, without producing an NFC list.
      for (std::size_t index = 0; index < entries.size(); ++index)
      {
        auto const shapeRes = detail::admitCreditShape(entries[index], index);

        if (!shapeRes)
        {
          return std::unexpected{shapeRes.error()};
        }

        if (auto textRes = detail::validateLibraryText(shapeRes->name, std::format("Credit entry {} name", index));
            !textRes)
        {
          return textRes;
        }

        if (auto textRes = detail::validateLibraryText(shapeRes->role, std::format("Credit entry {} role", index));
            !textRes)
        {
          return textRes;
        }
      }

      return {};
    }

    template<typename T>
    void writePod(std::span<std::byte> out, std::size_t offset, T const& value)
    {
      static_assert(std::is_trivially_copyable_v<T>);
      static_assert(std::is_standard_layout_v<T>);
      static_assert(alignof(T) <= kSerializedAlignmentBytes);

      AO_INVARIANT((offset % alignof(T)) == 0);
      AO_INVARIANT(offset + sizeof(T) <= out.size());

      std::memcpy(out.data() + offset, &value, sizeof(T));
    }

    template<typename Prepared>
    std::vector<std::byte> materializePrepared(Prepared const& prepared)
    {
      auto bytes = std::vector<std::byte>(prepared.size());
      prepared.writeTo(bytes);
      return bytes;
    }
  } // namespace

  //=============================================================================
  // TrackBuilder - factory methods
  //=============================================================================

  TrackBuilder TrackBuilder::makeEmpty()
  {
    return TrackBuilder{};
  }

  Result<TrackBuilder> TrackBuilder::fromCommittedCompleteView(TrackView const& view, DictionaryStore const& dictionary)
  {
    return fromCommittedView(view, dictionary, BaselineKind::Complete);
  }

  Result<TrackBuilder> TrackBuilder::fromCommittedHotView(TrackView const& view, DictionaryStore const& dictionary)
  {
    return fromCommittedView(view, dictionary, BaselineKind::HotOnly);
  }

  Result<TrackBuilder> TrackBuilder::fromCommittedView(TrackView const& view,
                                                       DictionaryStore const& dictionary,
                                                       BaselineKind baseline)
  {
    auto const resolve = [&dictionary](DictionaryId id)
    {
      auto const optText = dictionary.findText(id);

      if (!optText)
      {
        detail::throwLibraryError(
          Error::Code::InvalidInput, std::format("Track references unpublished dictionary ID {}", id.raw()));
      }

      return *optText;
    };

    try
    {
      return projectView(view, baseline, resolve);
    }
    catch (detail::LibraryException const& error)
    {
      return std::unexpected{error.error()};
    }
  }

  TrackBuilder TrackBuilder::projectView(TrackView const& view,
                                         BaselineKind baseline,
                                         std::function<std::string_view(DictionaryId)> const& resolve)
  {
    AO_EXPECTS(view.isHotValid());
    AO_EXPECTS(baseline == BaselineKind::HotOnly || view.isColdValid());

    auto builder = TrackBuilder{};
    builder._baselineKind = baseline;
    auto const optionalText = [&resolve](DictionaryId id)
    { return id == kInvalidDictionaryId ? std::string_view{} : resolve(id); };

    auto const metadata = view.metadata();
    builder.metadata()
      .year(metadata.year())
      .artist(optionalText(metadata.artistId()))
      .album(optionalText(metadata.albumId()))
      .albumArtist(optionalText(metadata.albumArtistId()))
      .composer(optionalText(metadata.composerId()))
      .genre(optionalText(metadata.genreId()));

    if (metadata.isTitleExplicitlyCleared())
    {
      builder.metadata().clearTitleExplicitly();
    }
    else
    {
      builder.metadata().title(metadata.title());
    }

    auto const prop = view.property();
    builder.property().sampleRate(prop.sampleRate()).codec(prop.codec()).bitDepth(prop.bitDepth());

    for (auto const tagId : view.tags())
    {
      builder.tags().add(resolve(tagId));
    }

    if (baseline == BaselineKind::HotOnly)
    {
      return builder;
    }

    builder.property()
      .uri(prop.uri())
      .duration(prop.duration())
      .bitrate(prop.bitrate())
      .channels(prop.channels())
      .segment(prop.segment());

    builder.metadata()
      .trackNumber(metadata.trackNumber())
      .trackTotal(metadata.trackTotal())
      .discNumber(metadata.discNumber())
      .discTotal(metadata.discTotal());

    auto const work = view.work();
    auto const performance = view.performance();
    builder.metadata()
      .work(optionalText(work.workId()))
      .movement(optionalText(work.movementId()))
      .movementNumber(work.movementNumber())
      .movementTotal(work.movementTotal())
      .recordingDate(performance.recordingDate());

    for (std::size_t index = 0; index < kCreditKindCount; ++index)
    {
      auto const kind = static_cast<CreditKind>(index);

      for (auto const& entry : performance.credits(kind))
      {
        builder._metadataBuilder._credits.push_back(
          {.name = resolve(entry.nameId), .kind = kind, .role = optionalText(entry.roleId)});
      }
    }

    for (auto const cover : view.coverArt())
    {
      builder.coverArt().add(cover.type, cover.resourceId);
    }

    for (auto const& [dictionaryId, value] : view.customMetadata())
    {
      builder.customMetadata().add(resolve(dictionaryId), value);
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
    _titleExplicitlyCleared = false;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::clearTitleExplicitly()
  {
    _title = {};
    _titleExplicitlyCleared = true;
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

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::recordingDate(RecordingDate date)
  {
    _recordingDate = date;
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::credits(std::span<CreditView const> entries)
  {
    auto replacement = std::vector<CreditView>{entries.begin(), entries.end()};
    _credits = std::move(replacement);
    return *this;
  }

  TrackBuilder::MetadataBuilder& TrackBuilder::MetadataBuilder::credits(std::span<Credit const> entries)
  {
    auto replacement = std::vector<CreditView>{};
    replacement.reserve(entries.size());

    for (auto const& entry : entries)
    {
      replacement.push_back({.name = entry.name, .kind = entry.kind, .role = entry.role});
    }

    _credits = std::move(replacement);
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

  std::chrono::milliseconds TrackBuilder::PropertyBuilder::duration() const
  {
    return _optSegment ? std::chrono::milliseconds{segmentDuration(_optSegment->frameCount, _sampleRate)
                                                     .value_or(TrackDuration{})}
                       : _duration;
  }

  TrackBuilder::PropertyBuilder& TrackBuilder::PropertyBuilder::segment(std::optional<TrackSegment> optSegment)
  {
    _optSegment = optSegment;
    return *this;
  }

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

  TrackBuilder::CoverArtBuilder& TrackBuilder::CoverArtBuilder::add(PictureType const type,
                                                                    ResourceDescriptor const& descriptor)
  {
    _entries.push_back({.type = type, .source = descriptor});
    return *this;
  }

  TrackBuilder::CoverArtBuilder& TrackBuilder::CoverArtBuilder::add(PictureType const type,
                                                                    ObservedResourceDescriptor const& observed)
  {
    _entries.push_back({.type = type, .source = observed});
    return *this;
  }

  TrackBuilder::CoverArtBuilder& TrackBuilder::CoverArtBuilder::erase(std::size_t index)
  {
    AO_EXPECTS(index < _entries.size());
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

  Result<> TrackBuilder::validateHotSerializable() const
  {
    try
    {
      auto const titleLengthRes = detail::normalizedLibraryTextSize(_metadataBuilder._title, "Track title");

      if (!titleLengthRes)
      {
        return std::unexpected{titleLengthRes.error()};
      }

      checkedUint16(*titleLengthRes, "Hot title length");

      for (auto const& [value, context] : {
             std::pair{_metadataBuilder._artist, std::string_view{"Track artist"}},
             std::pair{_metadataBuilder._album, std::string_view{"Track album"}},
             std::pair{_metadataBuilder._albumArtist, std::string_view{"Track album artist"}},
             std::pair{_metadataBuilder._composer, std::string_view{"Track composer"}},
             std::pair{_metadataBuilder._genre, std::string_view{"Track genre"}},
           })
      {
        if (auto textRes = detail::validateLibraryText(value, context); !textRes)
        {
          return textRes;
        }
      }

      auto normalizedTags = std::unordered_set<std::string>{};
      normalizedTags.reserve(_tagsBuilder._tagNames.size());

      for (auto const tag : _tagsBuilder._tagNames)
      {
        auto normalizedRes = detail::normalizeLibraryText(tag, "Track tag");

        if (!normalizedRes)
        {
          return std::unexpected{normalizedRes.error()};
        }

        normalizedTags.insert(std::move(*normalizedRes));
      }

      checkedPayloadBytes(normalizedTags.size(), sizeof(DictionaryId), "Hot tag payload length");
      return {};
    }
    catch (detail::LibraryException const& error)
    {
      return std::unexpected{error.error()};
    }
  }

  Result<> TrackBuilder::validateColdSerializable() const
  {
    AO_EXPECTS(_baselineKind != BaselineKind::HotOnly, "A hot-only TrackBuilder cannot serialize cold data");

    if (auto propertyRes = validateColdProperties(_propertyBuilder); !propertyRes)
    {
      return std::unexpected{propertyRes.error()};
    }

    auto const& metadata = _metadataBuilder;

    if (!metadata._recordingDate.isValid())
    {
      return makeError(Error::Code::InvalidInput, "Track recording date is invalid");
    }

    for (auto const& [value, context] : {
           std::pair{metadata._work, std::string_view{"Track work"}},
           std::pair{metadata._movement, std::string_view{"Track movement"}},
         })
    {
      if (auto textRes = detail::validateLibraryText(value, context); !textRes)
      {
        return std::unexpected{textRes.error()};
      }
    }

    auto const entryCount = _customMetadataBuilder._customPairs.size();
    auto normalizedKeys = std::unordered_set<std::string>{};
    std::size_t customValueSize = 0;

    if (entryCount != 0)
    {
      normalizedKeys.reserve(entryCount);
    }

    for (auto const& [key, value] : _customMetadataBuilder._customPairs)
    {
      auto normalizedKeyRes = detail::normalizeLibraryText(key, "Custom metadata key");

      if (!normalizedKeyRes)
      {
        return std::unexpected{normalizedKeyRes.error()};
      }

      if (!normalizedKeys.insert(std::move(*normalizedKeyRes)).second)
      {
        return makeError(Error::Code::InvalidInput, "Custom metadata keys must be unique after NFC normalization");
      }

      auto const valueLengthRes = detail::normalizedLibraryTextSize(value, "Custom metadata value");

      if (!valueLengthRes)
      {
        return std::unexpected{valueLengthRes.error()};
      }

      customValueSize += *valueLengthRes;
    }

    if (auto creditRes = validateCreditEntries(metadata._credits); !creditRes)
    {
      return creditRes;
    }

    try
    {
      // Input is valid before sizing, so a size rejection never hides invalid
      // input. Components are sized in their physical order and share one
      // record limit, so a rejection names the component that crosses it.
      auto recordSize = sizeof(TrackColdHeader);
      auto const addComponent = [&recordSize](std::size_t const size, std::string_view const component)
      {
        if (size > kTrackColdRecordSizeLimit - recordSize || align4(recordSize + size) > kTrackColdRecordSizeLimit)
        {
          throwColdRecordTooLarge(component);
        }

        recordSize = align4(recordSize + size);
      };

      if (!_coverArtBuilder._entries.empty())
      {
        addComponent(coldTableSize(0, _coverArtBuilder._entries.size(), sizeof(CoverArtEntry), "Cover art references"),
                     "Cover art references");
      }

      if (!metadata._work.empty() || !metadata._movement.empty() || metadata._movementNumber != 0 ||
          metadata._movementTotal != 0)
      {
        addComponent(sizeof(TrackWorkBlock), "Work");
      }

      if (entryCount != 0)
      {
        auto const tableSize =
          coldTableSize(sizeof(CustomMetadataBlockHeader), entryCount, sizeof(CustomMetadataEntry), "Custom metadata");

        if (customValueSize > kTrackColdRecordSizeLimit - tableSize)
        {
          throwColdRecordTooLarge("Custom metadata");
        }

        addComponent(tableSize + customValueSize, "Custom metadata");
      }

      if (metadata._recordingDate.isPresent() || !metadata._credits.empty())
      {
        constexpr auto kPerformanceComponent = std::string_view{"Recording date and credit references"};
        addComponent(
          coldTableSize(
            sizeof(TrackPerformanceBlock), metadata._credits.size(), sizeof(TrackCreditEntry), kPerformanceComponent),
          kPerformanceComponent);
      }

      if (_propertyBuilder.segment())
      {
        addComponent(sizeof(TrackSegmentBlock), "Segment");
      }

      addComponent(_propertyBuilder._uri.size(), "URI");
      return {};
    }
    catch (detail::LibraryException const& error)
    {
      return std::unexpected{error.error()};
    }
  }

  Result<std::vector<std::byte>> TrackBuilder::serializeHot(WriteTransaction& transaction) const
  {
    auto preparedRes = prepareHot(transaction);

    if (!preparedRes)
    {
      return std::unexpected{std::move(preparedRes.error())};
    }

    return materializePrepared(*preparedRes);
  }

  Result<std::vector<std::byte>> TrackBuilder::serializeCold(WriteTransaction& transaction,
                                                             ResourceStore const& resources) const
  {
    auto preparedRes = prepareCold(transaction, resources);

    if (!preparedRes)
    {
      return std::unexpected{std::move(preparedRes.error())};
    }

    return materializePrepared(*preparedRes);
  }

  Result<std::pair<std::vector<std::byte>, std::vector<std::byte>>> TrackBuilder::serialize(
    WriteTransaction& transaction,
    ResourceStore const& resources) const
  {
    auto preparedRes = prepare(transaction, resources);

    if (!preparedRes)
    {
      return std::unexpected{std::move(preparedRes.error())};
    }

    auto const& [hot, cold] = *preparedRes;
    return std::pair{materializePrepared(hot), materializePrepared(cold)};
  }

  //=============================================================================
  // Prepared structures for zero-copy serialization
  //=============================================================================

  void TrackBuilder::PreparedHot::setTitle(std::string title, bool explicitClear)
  {
    _title = std::move(title);
    _titleLength = checkedUint16(_title.size(), "Hot title length");
    AO_INVARIANT(!explicitClear || _titleLength == 0);
  }

  void TrackBuilder::PreparedHot::assignLayout(bool explicitClear)
  {
    _bloomFilter = computeBloomFilter(_tagIds);
    auto const tagCount = _tagIds.size();
    auto const tagBytes = checkedPayloadBytes(tagCount, sizeof(DictionaryId), "Hot tag payload length");
    _tagLengthAndFlags = packTrackHotTagLengthAndFlags(tagCount, explicitClear);
    _size = align4(sizeof(TrackHotHeader) + tagBytes + _titleLength);
  }

  void TrackBuilder::PreparedHot::writeTo(std::span<std::byte> out) const noexcept
  {
    // Exact size validation and alignment check
    AO_EXPECTS(out.size() == _size);
    AO_EXPECTS(utility::bytes::isAligned(out.data(), kSerializedAlignmentBytes));

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
               .tagLengthAndFlags = _tagLengthAndFlags,
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

  void TrackBuilder::PreparedCold::appendSegmentBlock(TrackSegment const& segment)
  {
    auto payload = std::vector<std::byte>(sizeof(TrackSegmentBlock));
    writePod(std::span<std::byte>{payload},
             0,
             TrackSegmentBlock{.startFrameLo = static_cast<std::uint32_t>(segment.startFrame),
                               .startFrameHi = static_cast<std::uint32_t>(segment.startFrame >> 32),
                               .frameCountLo = static_cast<std::uint32_t>(segment.frameCount),
                               .frameCountHi = static_cast<std::uint32_t>(segment.frameCount >> 32),
                               .ordinal = segment.ordinal});
    appendBlock(TrackColdBlockSlot::Segment, std::move(payload));
  }

  TrackBuilder::PreparedCold TrackBuilder::PreparedCold::relink(TrackView const& view, std::string_view uri)
  {
    auto prepared = PreparedCold{};
    auto const bytes = detail::TrackViewRawAccess::coldData(view);
    auto const& header = *utility::layout::view<TrackColdHeader>(bytes);
    prepared._duration = header.duration;
    prepared._bitrate = header.bitrate;
    prepared._trackNumber = header.trackNumber;
    prepared._trackTotal = header.trackTotal;
    prepared._discNumber = header.discNumber;
    prepared._discTotal = header.discTotal;
    prepared._channels = header.channels;

    for (std::size_t index = 0; index < kTrackColdBlockSlotCount; ++index)
    {
      auto const begin = header.blockOffsets[index];

      if (begin == 0)
      {
        continue;
      }

      auto end = header.uriOffset;

      for (auto next = index + 1; next < kTrackColdBlockSlotCount; ++next)
      {
        if (header.blockOffsets[next] != 0)
        {
          end = header.blockOffsets[next];
          break;
        }
      }

      auto const payload = bytes.subspan(begin, end - begin);
      prepared.appendBlock(static_cast<TrackColdBlockSlot>(index), {payload.begin(), payload.end()});
    }

    prepared.assignLayout(uri);
    prepared._uri = std::string{uri};
    return prepared;
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

  void TrackBuilder::PreparedCold::appendWorkBlock(std::uint16_t movementNumber, std::uint16_t movementTotal)
  {
    if (_workId == kInvalidDictionaryId && _movementId == kInvalidDictionaryId && movementNumber == 0 &&
        movementTotal == 0)
    {
      return;
    }

    auto payload = std::vector<std::byte>(sizeof(TrackWorkBlock), std::byte{0});
    writePod(std::span<std::byte>{payload},
             0,
             TrackWorkBlock{.workId = _workId,
                            .movementId = _movementId,
                            .movementNumber = movementNumber,
                            .movementTotal = movementTotal});
    appendBlock(TrackColdBlockSlot::Work, std::move(payload));
  }

  void TrackBuilder::PreparedCold::appendPerformanceBlock(RecordingDate date)
  {
    if (!date.isPresent() && _credits.empty())
    {
      return;
    }

    auto const payloadSize = sizeof(TrackPerformanceBlock) + (_credits.size() * sizeof(TrackCreditEntry));
    auto payload = std::vector<std::byte>(payloadSize, std::byte{0});
    writePod(std::span<std::byte>{payload}, 0, TrackPerformanceBlock{.recordingDate = date});
    auto offset = sizeof(TrackPerformanceBlock);

    for (auto const& entry : _credits)
    {
      writePod(std::span<std::byte>{payload}, offset, entry);
      offset += sizeof(TrackCreditEntry);
    }

    appendBlock(TrackColdBlockSlot::Performance, std::move(payload));
  }

  void TrackBuilder::PreparedCold::appendCustomMetadataBlock(
    std::vector<std::pair<DictionaryId, std::string>> const& resolvedPairs)
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
      AO_INVARIANT(slotIndex < kTrackColdKnownBlockSlotCount);
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

    if (recordSize < unalignedSize || recordSize > kTrackColdRecordSizeLimit)
    {
      throwColdRecordTooLarge("URI");
    }

    _size = recordSize;
  }

  void TrackBuilder::PreparedCold::snapshot(TrackBuilder const* builder)
  {
    auto const& metadata = builder->_metadataBuilder;
    auto const& property = builder->_propertyBuilder;

    _uri = std::string{property._uri};
    _duration = std::chrono::duration_cast<TrackDuration>(property.duration());
    _bitrate = property._bitrate;
    _trackNumber = metadata._trackNumber;
    _trackTotal = metadata._trackTotal;
    _discNumber = metadata._discNumber;
    _discTotal = metadata._discTotal;
    _channels = property._channels;
  }

  void TrackBuilder::PreparedCold::writeTo(std::span<std::byte> out) const noexcept
  {
    // Exact size validation and alignment check
    AO_EXPECTS(out.size() == _size);
    AO_EXPECTS(utility::bytes::isAligned(out.data(), kSerializedAlignmentBytes));

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
      AO_INVARIANT(offset == _blockOffsets[trackColdBlockSlotIndex(block.slot)]);

      if (!block.payload.empty())
      {
        std::memcpy(out.data() + offset, block.payload.data(), block.payload.size());
        offset += block.payload.size();
      }

      offset = align4(offset);
    }

    AO_INVARIANT(offset == _uriOffset);

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
    WriteTransaction& transaction,
    ResourceStore const& resources) const
  {
    if (auto validationRes = validateHotSerializable(); !validationRes)
    {
      return std::unexpected{validationRes.error()};
    }

    if (auto validationRes = validateColdSerializable(); !validationRes)
    {
      return std::unexpected{validationRes.error()};
    }

    return detail::TrackPreparation::stageValidated(*this, transaction, resources).finish();
  }

  Result<TrackBuilder::PreparedHot> TrackBuilder::prepareHot(WriteTransaction& transaction) const
  {
    if (auto validationRes = validateHotSerializable(); !validationRes)
    {
      return std::unexpected{validationRes.error()};
    }

    return detail::TrackPreparation::stageHotValidated(*this, transaction).finish();
  }

  Result<TrackBuilder::PreparedCold> TrackBuilder::prepareCold(WriteTransaction& transaction,
                                                               ResourceStore const& resources) const
  {
    if (auto validationRes = validateColdSerializable(); !validationRes)
    {
      return std::unexpected{validationRes.error()};
    }

    return detail::TrackPreparation::stageColdValidated(*this, transaction, resources).finish();
  }
} // namespace ao::library
