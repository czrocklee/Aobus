// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/TrackView.h>

#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/PictureType.h>
#include <ao/library/CoverArt.h>
#include <ao/library/Credits.h>
#include <ao/library/RecordingDate.h>
#include <ao/library/TrackLayout.h>
#include <ao/utility/ByteView.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace ao::library
{
  namespace
  {
    constexpr std::size_t kSerializedAlignmentBytes = 4;
  } // namespace

  RecordingDate PerformanceView::recordingDate() const noexcept
  {
    return empty() ? RecordingDate{} : utility::layout::view<TrackPerformanceBlock>(_payload)->recordingDate;
  }

  std::span<TrackCreditEntry const> PerformanceView::credits() const noexcept
  {
    if (empty())
    {
      return {};
    }

    return utility::layout::viewArray<TrackCreditEntry>(_payload.subspan(sizeof(TrackPerformanceBlock)));
  }

  std::span<TrackCreditEntry const> PerformanceView::credits(CreditKind kind) const noexcept
  {
    AO_EXPECTS(isValidCreditKind(kind));

    if (empty())
    {
      return {};
    }

    auto const& counts = utility::layout::view<TrackPerformanceBlock>(_payload)->sectionCounts;
    auto const index = static_cast<std::size_t>(kind);
    std::size_t begin = 0;

    for (std::size_t previous = 0; previous < index; ++previous)
    {
      begin += counts[previous];
    }

    auto const entries = credits();
    auto const count = index < counts.size() ? static_cast<std::size_t>(counts[index]) : entries.size() - begin;
    return entries.subspan(begin, count);
  }

  CoverArt CoverArtProxy::at(std::uint16_t index) const noexcept
  {
    AO_EXPECTS(index < count());
    auto const& entry = _entries[index];
    return {.resourceId = entry.id, .type = static_cast<PictureType>(entry.type)};
  }

  std::optional<CoverArt> CoverArtProxy::primary() const noexcept
  {
    if (empty())
    {
      return std::nullopt;
    }

    if (auto it = std::ranges::find(*this, PictureType::FrontCover, &CoverArt::type); it != end())
    {
      return *it;
    }

    return *begin();
  }

  CoverArtProxy::Iterator CoverArtProxy::begin() const
  {
    return Iterator{_entries.data()};
  }

  CoverArtProxy::Iterator CoverArtProxy::end() const
  {
    auto const* end = _entries.data();

    if (end != nullptr)
    {
      end += _entries.size();
    }

    return Iterator{end};
  }

  CustomMetadataProxy::Iterator CustomMetadataProxy::begin() const
  {
    return {entries().data(), _payload};
  }

  CustomMetadataProxy::Iterator CustomMetadataProxy::end() const
  {
    auto customEntries = entries();
    auto const* end = customEntries.data();

    if (end != nullptr)
    {
      end += customEntries.size();
    }

    return {end, _payload};
  }

  std::optional<std::string_view> CustomMetadataProxy::get(DictionaryId dictionaryId) const noexcept
  {
    constexpr std::size_t kSearchThreshold = 64;
    auto customEntries = entries();

    if (customEntries.size() < kSearchThreshold)
    {
      if (auto it = std::ranges::find(customEntries, dictionaryId, &Entry::keyId); it != customEntries.end())
      {
        return value(_payload, *it);
      }

      return std::nullopt;
    }

    if (auto it = std::ranges::lower_bound(customEntries, dictionaryId, {}, &Entry::keyId);
        it != customEntries.end() && it->keyId == dictionaryId)
    {
      return value(_payload, *it);
    }

    return std::nullopt;
  }

  bool CustomMetadataProxy::contains(DictionaryId dictionaryId) const noexcept
  {
    constexpr std::size_t kSearchThreshold = 64;
    auto customEntries = entries();

    if (customEntries.size() < kSearchThreshold)
    {
      return std::ranges::find(customEntries, dictionaryId, &Entry::keyId) != customEntries.end();
    }

    auto it = std::ranges::lower_bound(customEntries, dictionaryId, {}, &Entry::keyId);
    return it != customEntries.end() && it->keyId == dictionaryId;
  }

  std::span<CustomMetadataProxy::Entry const> CustomMetadataProxy::entries() const noexcept
  {
    if (_payload.empty())
    {
      return {};
    }

    auto const entryBytes = static_cast<std::size_t>(count()) * sizeof(Entry);
    return utility::layout::viewArray<Entry>(_payload.subspan(sizeof(CustomMetadataBlockHeader), entryBytes));
  }

  std::string_view CustomMetadataProxy::value(std::span<std::byte const> payload, Entry const& entry) noexcept
  {
    auto const valueOffset = static_cast<std::size_t>(entry.valueOffset);
    auto const valueLength = static_cast<std::size_t>(entry.valueLength);

    // Per-entry clamp: entry ranges are data, not gated structure, so one
    // bounds check per access keeps garbage entries memory-safe.
    if (valueOffset > payload.size() || valueLength > payload.size() - valueOffset)
    {
      return {};
    }

    return utility::bytes::stringView(payload.subspan(valueOffset, valueLength));
  }

  std::string_view TrackView::MetadataProxy::title() const noexcept
  {
    auto const& header = _track.hotHeader();

    return utility::bytes::stringView(
      _track._hotData.subspan(sizeof(TrackHotHeader) + header.tagLength, header.titleLength));
  }

  DictionaryId TrackView::TagProxy::id(std::uint16_t index) const noexcept
  {
    AO_EXPECTS(index < count());
    return _tagIds[index];
  }

  bool TrackView::TagProxy::has(DictionaryId tagIdToCheck) const noexcept
  {
    return std::ranges::contains(*this, tagIdToCheck);
  }

  TrackView::TagProxy TrackView::tags() const noexcept
  {
    auto const& header = hotHeader();

    return TagProxy{
      header.tagBloom,
      utility::layout::viewArray<DictionaryId>(_hotData.subspan(sizeof(TrackHotHeader), header.tagLength))};
  }

  TrackHotHeader const* TrackView::gateHot(std::span<std::byte const> hotData) noexcept
  {
    auto const* header = utility::bytes::tryLayout<TrackHotHeader>(hotData);

    if (header == nullptr || sizeof(TrackHotHeader) + header->tagLength + header->titleLength > hotData.size() ||
        header->tagLength % sizeof(DictionaryId) != 0)
    {
      return nullptr;
    }

    return header;
  }

  TrackHotHeader const& TrackView::hotHeader() const noexcept
  {
    AO_EXPECTS(_hotHeader != nullptr);
    return *_hotHeader;
  }

  TrackColdHeader const& TrackView::coldHeader() const noexcept
  {
    return *requiredColdIndex().header;
  }

  TrackView::ColdIndex const& TrackView::requiredColdIndex() const noexcept
  {
    auto const& index = coldIndex();
    AO_EXPECTS(index.header != nullptr);
    return index;
  }

  /**
   * O(1) cold gate: header fits and is aligned, the URI range stays inside
   * the record, and present known block slots are aligned, strictly
   * increasing, and end before the URI. Slot payloads get one size check
   * each so the views can trust their slices. Unknown trailing slots are
   * rejected; bytes beyond the URI and semantic invariants (no gaps, zeroed
   * padding, sorted custom keys) are the write side's job and are only
   * re-checked by detail::TrackColdReader.
   */
  TrackView::ColdIndex const& TrackView::scanColdIndex() const noexcept
  {
    auto& index = _coldIndex;
    index.scanned = true;

    auto const* header = utility::bytes::tryLayout<TrackColdHeader>(_coldData);

    if (header == nullptr)
    {
      return index;
    }

    auto const recordSize = _coldData.size();
    auto const uriOffset = static_cast<std::size_t>(header->uriOffset);
    auto const uriLength = static_cast<std::size_t>(header->uriLength);

    if (uriOffset < sizeof(TrackColdHeader) || uriOffset > recordSize || uriLength > recordSize - uriOffset)
    {
      return index;
    }

    for (std::size_t i = kTrackColdKnownBlockSlotCount; i < kTrackColdBlockSlotCount; ++i)
    {
      if (header->blockOffsets[i] != 0)
      {
        return index;
      }
    }

    auto begins = std::array<std::size_t, kTrackColdKnownBlockSlotCount>{};
    std::size_t previousBegin = 0;

    for (std::size_t i = 0; i < kTrackColdKnownBlockSlotCount; ++i)
    {
      auto const offset = static_cast<std::size_t>(header->blockOffsets[i]);
      begins[i] = offset;

      if (offset == 0)
      {
        continue;
      }

      if (offset < sizeof(TrackColdHeader) || offset >= uriOffset || offset % kSerializedAlignmentBytes != 0 ||
          offset <= previousBegin)
      {
        return index;
      }

      previousBegin = offset;
    }

    auto payloads = std::array<std::span<std::byte const>, kTrackColdKnownBlockSlotCount>{};
    std::size_t endOffset = uriOffset;

    for (std::size_t slotIndex = kTrackColdKnownBlockSlotCount; slotIndex > 0; --slotIndex)
    {
      std::size_t const beginOffset = begins[slotIndex - 1];

      if (beginOffset == 0)
      {
        continue;
      }

      payloads[slotIndex - 1] = _coldData.subspan(beginOffset, endOffset - beginOffset);
      endOffset = beginOffset;
    }

    auto const workPayload = payloads[trackColdBlockSlotIndex(TrackColdBlockSlot::Work)];

    if (!workPayload.empty() && workPayload.size() != sizeof(TrackWorkBlock))
    {
      return index;
    }

    auto const performancePayload = payloads[trackColdBlockSlotIndex(TrackColdBlockSlot::Performance)];

    if (!performancePayload.empty())
    {
      auto const* block = utility::bytes::tryLayout<TrackPerformanceBlock>(performancePayload);

      if (block == nullptr ||
          (performancePayload.size() - sizeof(TrackPerformanceBlock)) % sizeof(TrackCreditEntry) != 0)
      {
        return index;
      }

      auto const count = (performancePayload.size() - sizeof(TrackPerformanceBlock)) / sizeof(TrackCreditEntry);
      auto const classified =
        static_cast<std::size_t>(block->sectionCounts[0]) + block->sectionCounts[1] + block->sectionCounts[2];

      if (classified > count)
      {
        return index;
      }
    }

    auto const customPayload = payloads[trackColdBlockSlotIndex(TrackColdBlockSlot::CustomMetadata)];

    if (!customPayload.empty())
    {
      auto const* customHeader = utility::bytes::tryLayout<CustomMetadataBlockHeader>(customPayload);

      if (customHeader == nullptr || static_cast<std::size_t>(customHeader->entryCount) * sizeof(CustomMetadataEntry) >
                                       customPayload.size() - sizeof(CustomMetadataBlockHeader))
      {
        return index;
      }
    }

    auto coverPayload = payloads[trackColdBlockSlotIndex(TrackColdBlockSlot::CoverArt)];
    coverPayload = coverPayload.first(coverPayload.size() - (coverPayload.size() % sizeof(CoverArtEntry)));

    index.uri = _coldData.subspan(uriOffset, uriLength);
    index.cover = coverPayload;
    index.work = workPayload;
    index.custom = customPayload;
    index.performance = performancePayload;
    index.header = header;
    return index;
  }

  CustomMetadataProxy::Iterator& CustomMetadataProxy::Iterator::operator++() noexcept
  {
    ++_entry;
    return *this;
  }

  CustomMetadataProxy::Iterator CustomMetadataProxy::Iterator::operator++(std::int32_t) noexcept
  {
    auto result = *this;
    ++_entry;
    return result;
  }

  CoverArtProxy::Iterator& CoverArtProxy::Iterator::operator++()
  {
    ++_entry;
    return *this;
  }

  CoverArtProxy::Iterator CoverArtProxy::Iterator::operator++(std::int32_t)
  {
    auto result = *this;
    ++_entry;
    return result;
  }
} // namespace ao::library
