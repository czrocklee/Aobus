// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackRecordValidation.h"

#include "LibraryUriValidation.h"
#include "TextAdmission.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackView.h>
#include <ao/library/detail/TrackColdReader.h>
#include <ao/utility/ByteView.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <tuple>
#include <unordered_set>

namespace ao::library
{
  namespace
  {
    constexpr std::size_t kSerializedAlignmentBytes = 4;
    constexpr std::size_t kSerializedAlignmentMask = kSerializedAlignmentBytes - 1;
    constexpr std::uint32_t kBloomBitMask = 31;

    constexpr std::size_t align4(std::size_t size) noexcept
    {
      return (size + kSerializedAlignmentMask) & ~kSerializedAlignmentMask;
    }

    bool isZero(std::byte value) noexcept
    {
      return value == std::byte{0};
    }
  } // namespace

  Result<> validateSerializedHotTrack(std::span<std::byte const> bytes)
  {
    auto const view = TrackView{bytes, {}};
    if (bytes.empty() || bytes.size() % kSerializedAlignmentBytes != 0 ||
        !utility::bytes::isAligned(bytes.data(), kSerializedAlignmentBytes) || !view.isHotValid())
    {
      return makeError(Error::Code::CorruptData, "Hot Track record has an invalid structural layout");
    }

    auto const* header = utility::layout::view<TrackHotHeader>(bytes);
    auto const logicalSize = sizeof(TrackHotHeader) + static_cast<std::size_t>(header->tagLength) + header->titleLength;

    if (align4(logicalSize) != bytes.size() || !std::ranges::all_of(bytes.subspan(logicalSize), isZero))
    {
      return makeError(Error::Code::CorruptData, "Hot Track record has a non-canonical size or padding");
    }

    auto const tagBytes = bytes.subspan(sizeof(TrackHotHeader), header->tagLength);
    auto const tags = utility::layout::viewArray<DictionaryId>(tagBytes);
    std::uint32_t expectedBloom = 0;
    auto seenTags = std::unordered_set<DictionaryId>{};
    seenTags.reserve(tags.size());

    for (auto const id : tags)
    {
      if (id == kInvalidDictionaryId)
      {
        return makeError(Error::Code::CorruptData, "Hot Track record contains an invalid tag ID");
      }
      if (!seenTags.insert(id).second)
      {
        return makeError(Error::Code::CorruptData, "Hot Track record contains a duplicate tag ID");
      }

      expectedBloom |= std::uint32_t{1} << (id.raw() & kBloomBitMask);
    }

    if (header->tagBloom != expectedBloom)
    {
      return makeError(Error::Code::CorruptData, "Hot Track record tag bloom does not match its tag IDs");
    }

    auto const title = view.metadata().title();
    if (auto titleRes = detail::validatePersistedLibraryText(title, "Track title"); !titleRes)
    {
      return std::unexpected{titleRes.error()};
    }

    return {};
  }

  Result<> validateSerializedColdTrack(std::span<std::byte const> bytes)
  {
    auto const reader = detail::TrackColdReader{bytes};

    if (!reader.isValid())
    {
      return makeError(Error::Code::CorruptData, "Cold Track record has a non-canonical structural layout");
    }

    if (auto const uri = reader.uri(); !detail::isCanonicalLibraryUri(uri))
    {
      return makeError(Error::Code::CorruptData, "Cold Track record contains a non-canonical library URI");
    }

    for (auto const& [keyId, value] : reader.custom())
    {
      std::ignore = keyId;
      if (auto valueRes = detail::validatePersistedLibraryText(value, "Custom metadata value"); !valueRes)
      {
        return std::unexpected{valueRes.error()};
      }
    }

    return {};
  }

  Result<> validateSerializedTrackReferences(std::span<std::byte const> const hotBytes,
                                             std::span<std::byte const> const coldBytes,
                                             std::size_t const dictionarySize)
  {
    if (auto const validationRes = validateSerializedHotTrack(hotBytes); !validationRes)
    {
      return validationRes;
    }

    if (auto const validationRes = validateSerializedColdTrack(coldBytes); !validationRes)
    {
      return validationRes;
    }

    auto const validReference = [dictionarySize](DictionaryId const id, bool const optional) noexcept
    {
      return (optional && id == kInvalidDictionaryId) ||
             (id != kInvalidDictionaryId && static_cast<std::size_t>(id.raw()) <= dictionarySize);
    };
    auto const view = TrackView{hotBytes, coldBytes};
    auto const metadata = view.metadata();

    for (auto const id :
         {metadata.artistId(), metadata.albumId(), metadata.genreId(), metadata.albumArtistId(), metadata.composerId()})
    {
      if (!validReference(id, true))
      {
        return makeError(Error::Code::CorruptData, "Track record contains an unresolved metadata dictionary id");
      }
    }

    for (auto const id : view.tags())
    {
      if (!validReference(id, false))
      {
        return makeError(Error::Code::CorruptData, "Track record contains an unresolved tag dictionary id");
      }
    }

    auto const classical = view.classical();

    for (auto const id : {classical.workId(),
                          classical.movementId(),
                          classical.conductorId(),
                          classical.ensembleId(),
                          classical.soloistId()})
    {
      if (!validReference(id, true))
      {
        return makeError(Error::Code::CorruptData, "Track record contains an unresolved classical dictionary id");
      }
    }

    for (auto const& [id, value] : view.customMetadata())
    {
      std::ignore = value;

      if (!validReference(id, false))
      {
        return makeError(Error::Code::CorruptData, "Track record contains an unresolved custom dictionary id");
      }
    }

    return {};
  }
} // namespace ao::library
