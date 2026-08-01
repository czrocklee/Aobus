// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackRecordValidation.h"

#include "LibraryUriValidation.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackView.h>
#include <ao/library/detail/TrackColdReader.h>
#include <ao/utility/ByteView.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <tuple>

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
    if (auto const view = TrackView{bytes, {}}; bytes.empty() || bytes.size() % kSerializedAlignmentBytes != 0 ||
                                                !utility::bytes::isAligned(bytes.data(), kSerializedAlignmentBytes) ||
                                                !view.isHotValid())
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

    for (auto const id : tags)
    {
      if (id == kInvalidDictionaryId)
      {
        return makeError(Error::Code::CorruptData, "Hot Track record contains an invalid tag ID");
      }

      expectedBloom |= std::uint32_t{1} << (id.raw() & kBloomBitMask);
    }

    if (header->tagBloom != expectedBloom)
    {
      return makeError(Error::Code::CorruptData, "Hot Track record tag bloom does not match its tag IDs");
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

    return {};
  }

  Result<> validateSerializedTrackReferences(std::span<std::byte const> const hotBytes,
                                             std::span<std::byte const> const coldBytes,
                                             std::size_t const dictionarySize)
  {
    if (auto const validation = validateSerializedHotTrack(hotBytes); !validation)
    {
      return validation;
    }

    if (auto const validation = validateSerializedColdTrack(coldBytes); !validation)
    {
      return validation;
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
