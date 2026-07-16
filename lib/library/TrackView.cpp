// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/PictureType.h>
#include <ao/library/CoverArt.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackView.h>
#include <ao/utility/ByteView.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace ao::library
{
  namespace
  {
    constexpr std::size_t kSerializedAlignmentBytes = 4;
  } // namespace

  /**
   * O(1) cold gate: header fits and is aligned, the URI range stays inside
   * the record, and present known block slots are aligned, strictly
   * increasing, and end before the URI. Slot payloads get one size check
   * each so the proxies can trust their slices. Unknown trailing slots and
   * bytes beyond the URI are ignored; semantic invariants (no gaps, zeroed
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

    auto payloadForSlot = [&](TrackColdBlockSlot slot) noexcept
    {
      auto const slotIndex = trackColdBlockSlotIndex(slot);

      if (begins[slotIndex] == 0)
      {
        return std::span<std::byte const>{};
      }

      std::size_t endOffset = uriOffset;

      for (auto nextIndex = slotIndex + 1; nextIndex < kTrackColdKnownBlockSlotCount; ++nextIndex)
      {
        if (begins[nextIndex] != 0)
        {
          endOffset = begins[nextIndex];
          break;
        }
      }

      return _coldData.subspan(begins[slotIndex], endOffset - begins[slotIndex]);
    };

    auto classicalPayload = payloadForSlot(TrackColdBlockSlot::Classical);

    if (!classicalPayload.empty())
    {
      if (classicalPayload.size() < sizeof(TrackClassicalBlock))
      {
        return index;
      }

      classicalPayload = classicalPayload.first(sizeof(TrackClassicalBlock));
    }

    auto const customPayload = payloadForSlot(TrackColdBlockSlot::CustomMetadata);

    if (!customPayload.empty())
    {
      auto const* customHeader = utility::bytes::tryLayout<CustomMetadataBlockHeader>(customPayload);

      if (customHeader == nullptr || static_cast<std::size_t>(customHeader->entryCount) * sizeof(CustomMetadataEntry) >
                                       customPayload.size() - sizeof(CustomMetadataBlockHeader))
      {
        return index;
      }
    }

    auto coverPayload = payloadForSlot(TrackColdBlockSlot::CoverArt);
    coverPayload = coverPayload.first(coverPayload.size() - (coverPayload.size() % sizeof(CoverArtEntry)));

    index.uri = _coldData.subspan(uriOffset, uriLength);
    index.cover = coverPayload;
    index.classical = classicalPayload;
    index.custom = customPayload;
    index.header = header;
    return index;
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

  bool TrackView::TagProxy::has(DictionaryId tagIdToCheck) const noexcept
  {
    return std::ranges::contains(*this, tagIdToCheck);
  }
} // namespace ao::library
