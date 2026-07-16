// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/PictureType.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/detail/TrackColdReader.h>
#include <ao/utility/ByteView.h>

#include <gsl-lite/gsl-lite.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

namespace ao::library::detail
{
  namespace
  {
    constexpr std::size_t kSerializedAlignmentBytes = 4;
    constexpr std::size_t kSerializedAlignmentMask = kSerializedAlignmentBytes - 1;

    constexpr std::size_t align4(std::size_t size) noexcept
    {
      return (size + kSerializedAlignmentMask) & ~kSerializedAlignmentMask;
    }

    struct ColdRecordLayout final
    {
      TrackColdHeader const* header = nullptr;
      std::size_t uriOffset = 0;
    };

    bool isValidCoverPayload(std::span<std::byte const> payload) noexcept
    {
      if (payload.empty() || payload.size() % sizeof(CoverArtEntry) != 0)
      {
        return false;
      }

      if (utility::bytes::tryLayout<CoverArtEntry>(payload) == nullptr)
      {
        return false;
      }

      auto const entries = utility::layout::viewArray<CoverArtEntry>(payload);

      return std::ranges::all_of(entries,
                                 [](CoverArtEntry const& entry)
                                 {
                                   return entry.id != kInvalidResourceId &&
                                          entry.type <= static_cast<std::uint8_t>(PictureType::PublisherLogo) &&
                                          std::ranges::all_of(
                                            entry.reserved, [](std::uint8_t value) { return value == 0; });
                                 });
    }

    bool isValidClassicalPayload(std::span<std::byte const> payload) noexcept
    {
      return payload.size() == sizeof(TrackClassicalBlock) &&
             utility::bytes::tryLayout<TrackClassicalBlock>(payload) != nullptr;
    }

    bool isValidCustomPayload(std::span<std::byte const> payload) noexcept
    {
      auto const* header = utility::bytes::tryLayout<CustomMetadataBlockHeader>(payload);

      if (header == nullptr)
      {
        return false;
      }

      auto const logicalSize = static_cast<std::size_t>(header->payloadLength);

      if (header->reserved != 0 || logicalSize < sizeof(CustomMetadataBlockHeader) || logicalSize > payload.size() ||
          align4(logicalSize) != payload.size())
      {
        return false;
      }

      if (!std::ranges::all_of(payload.subspan(logicalSize), [](std::byte value) { return value == std::byte{0}; }))
      {
        return false;
      }

      auto const entryCount = static_cast<std::size_t>(header->entryCount);
      auto const entryBytes = entryCount * sizeof(CustomMetadataEntry);
      auto const expectedValueOffset = sizeof(CustomMetadataBlockHeader) + entryBytes;
      auto const logicalPayload = payload.first(logicalSize);

      if (expectedValueOffset > logicalSize || header->valueOffset != expectedValueOffset)
      {
        return false;
      }

      auto const entrySpan = logicalPayload.subspan(sizeof(CustomMetadataBlockHeader), entryBytes);

      if (!entrySpan.empty() && utility::bytes::tryLayout<CustomMetadataEntry>(entrySpan) == nullptr)
      {
        return false;
      }

      auto const entries = utility::layout::viewArray<CustomMetadataEntry>(entrySpan);
      auto expectedValueEnd = static_cast<std::size_t>(header->valueOffset);

      for (std::size_t i = 0; i < entries.size(); ++i)
      {
        auto const& entry = entries[i];
        auto const valueOffset = static_cast<std::size_t>(entry.valueOffset);
        auto const valueLength = static_cast<std::size_t>(entry.valueLength);

        if (entry.keyId == kInvalidDictionaryId || valueOffset != expectedValueEnd || valueOffset > logicalSize ||
            valueLength > logicalSize - valueOffset)
        {
          return false;
        }

        expectedValueEnd = valueOffset + valueLength;

        if (i > 0 && entries[i - 1].keyId > entry.keyId)
        {
          return false;
        }
      }

      return expectedValueEnd == logicalSize;
    }

    std::optional<ColdRecordLayout> readColdRecordLayout(std::span<std::byte const> coldBytes) noexcept
    {
      auto const* header = utility::bytes::tryLayout<TrackColdHeader>(coldBytes);

      if (header == nullptr || header->reserved8 != 0 || coldBytes.size() % kSerializedAlignmentBytes != 0 ||
          !utility::bytes::isAligned(coldBytes.data(), kSerializedAlignmentBytes))
      {
        return std::nullopt;
      }

      auto const uriOffset = static_cast<std::size_t>(header->uriOffset);
      auto const uriLength = static_cast<std::size_t>(header->uriLength);

      if (uriOffset < sizeof(TrackColdHeader) || uriOffset % kSerializedAlignmentBytes != 0 ||
          uriOffset > coldBytes.size() || uriLength > coldBytes.size() - uriOffset)
      {
        return std::nullopt;
      }

      auto const uriEnd = uriOffset + uriLength;

      if (auto const recordSize = align4(uriEnd); recordSize < uriEnd || recordSize != coldBytes.size() ||
                                                  recordSize > std::numeric_limits<std::uint16_t>::max())
      {
        return std::nullopt;
      }

      if (!std::ranges::all_of(coldBytes.subspan(uriEnd), [](std::byte value) { return value == std::byte{0}; }))
      {
        return std::nullopt;
      }

      return ColdRecordLayout{.header = header, .uriOffset = uriOffset};
    }

    bool hasValidBlockOffsets(TrackColdHeader const& header, std::size_t uriOffset) noexcept
    {
      std::size_t presentCount = 0;
      std::size_t previousOffset = 0;

      for (std::size_t i = 0; i < kTrackColdBlockSlotCount; ++i)
      {
        auto const offset = static_cast<std::size_t>(header.blockOffsets[i]);

        if (i >= kTrackColdKnownBlockSlotCount)
        {
          if (offset != 0)
          {
            return false;
          }

          continue;
        }

        if (offset == 0)
        {
          continue;
        }

        if (offset < sizeof(TrackColdHeader) || offset >= uriOffset || offset % kSerializedAlignmentBytes != 0)
        {
          return false;
        }

        if ((presentCount == 0 && offset != sizeof(TrackColdHeader)) ||
            (previousOffset != 0 && offset <= previousOffset))
        {
          return false;
        }

        previousOffset = offset;
        ++presentCount;
      }

      return presentCount != 0 || uriOffset == sizeof(TrackColdHeader);
    }

    std::span<std::byte const> payloadForSlot(TrackColdHeader const& header,
                                              std::span<std::byte const> coldBytes,
                                              std::size_t uriOffset,
                                              TrackColdBlockSlot slot) noexcept
    {
      auto const slotIndex = trackColdBlockSlotIndex(slot);
      auto const offset = static_cast<std::size_t>(header.blockOffsets[slotIndex]);

      if (offset == 0)
      {
        return {};
      }

      std::size_t endOffset = uriOffset;

      for (auto nextIndex = slotIndex + 1; nextIndex < kTrackColdKnownBlockSlotCount; ++nextIndex)
      {
        if (header.blockOffsets[nextIndex] != 0)
        {
          endOffset = static_cast<std::size_t>(header.blockOffsets[nextIndex]);
          break;
        }
      }

      return coldBytes.subspan(offset, endOffset - offset);
    }

    bool hasValidPayloads(std::span<std::byte const> coverPayload,
                          std::span<std::byte const> classicalPayload,
                          std::span<std::byte const> customPayload) noexcept
    {
      return (coverPayload.empty() || isValidCoverPayload(coverPayload)) &&
             (classicalPayload.empty() || isValidClassicalPayload(classicalPayload)) &&
             (customPayload.empty() || isValidCustomPayload(customPayload));
    }
  } // namespace

  TrackColdReader::TrackColdReader(std::span<std::byte const> coldBytes) noexcept
    : _coldBytes{coldBytes}
  {
    auto const optLayout = readColdRecordLayout(coldBytes);

    if (!optLayout)
    {
      return;
    }

    auto const& header = *optLayout->header;

    if (!hasValidBlockOffsets(header, optLayout->uriOffset))
    {
      return;
    }

    auto const coverPayload = payloadForSlot(header, coldBytes, optLayout->uriOffset, TrackColdBlockSlot::CoverArt);
    auto const classicalPayload =
      payloadForSlot(header, coldBytes, optLayout->uriOffset, TrackColdBlockSlot::Classical);
    auto const customPayload =
      payloadForSlot(header, coldBytes, optLayout->uriOffset, TrackColdBlockSlot::CustomMetadata);

    if (!hasValidPayloads(coverPayload, classicalPayload, customPayload))
    {
      return;
    }

    _header = optLayout->header;
    _coverPayload = coverPayload;
    _classicalPayload = classicalPayload;
    _customPayload = customPayload;
    _valid = true;
  }

  TrackColdHeader const& TrackColdReader::header() const noexcept
  {
    gsl_Expects(_header != nullptr);
    return *_header;
  }

  std::string_view TrackColdReader::uri() const noexcept
  {
    if (!_valid)
    {
      return {};
    }

    return utility::bytes::stringView(
      _coldBytes.subspan(static_cast<std::size_t>(_header->uriOffset), static_cast<std::size_t>(_header->uriLength)));
  }
} // namespace ao::library::detail
