// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ratio>

namespace ao::library
{
  /**
   * Storage representation for a track's duration: a millisecond span backed by a 32-bit
   * signed integer. Sized to fit the on-disk header (4 bytes, same byte layout as the
   * previous std::uint32_t); the signed int32 range (~24.8 days) is far beyond any real
   * track. Widens implicitly and losslessly to std::chrono::milliseconds at use sites.
   */
  using TrackDuration = std::chrono::duration<std::int32_t, std::milli>;
  /**
   * TrackHotHeader - POD struct for hot track storage.
   * Hot fields are used for fast filtering/sorting operations.
   *
   * Layout uses strictly descending member sizes (4→2→1) for natural alignment.
   * Total size: 36 bytes with 4-byte alignment. The members end exactly on a 4-byte
   * boundary (7×4 + 3×2 + 2×1 = 36), so there is no internal or trailing padding and
   * every byte is a real field — on-disk serialization is fully deterministic.
   *
   * Layout:
   *   ┌─────────────────────────────────────┐  ← hot data begin
   *   │        TrackHotHeader (36B)         │
   *   │  tagBloom, sampleRate,              │
   *   │  artistId, albumId,                 │
   *   │  genreId, albumArtistId,            │
   *   │  composerId, year,                  │
   *   │  titleLength, tagLength,            │
   *   │  bitDepth, codec                    │
   *   ├─────────────────────────────────────┤  ← tags begin = header + sizeof(header)
   *   │  tag ID 1 (4B)                      │
   *   │  tag ID 2 (4B)                      │
   *   │  ... (tagLength bytes total)        │
   *   ├─────────────────────────────────────┤  ← title begin = tags begin + tagLength
   *   │  title... (titleLength bytes)       │
   *   │  '\0'                               │
   *   └─────────────────────────────────────┘  ← hot data end
   */
  struct TrackHotHeader final
  {
    // 4-byte section
    std::uint32_t tagBloom{};     // Bloom filter for tags (32-bit)
    DictionaryId artistId{};      // Dictionary ID for artist
    DictionaryId albumId{};       // Dictionary ID for album
    DictionaryId genreId{};       // Dictionary ID for genre
    DictionaryId albumArtistId{}; // Dictionary ID for album artist
    DictionaryId composerId{};    // Dictionary ID for composer
    SampleRate sampleRate{};      // Sample rate in Hz

    // 2-byte section
    std::uint16_t year{};        // Release year
    std::uint16_t titleLength{}; // Length of title string
    std::uint16_t tagLength{};   // Length of tags blob in bytes

    // 1-byte section
    BitDepth bitDepth{}; // Bits per sample
    AudioCodec codec{};  // Audio codec
  };

  // Binary layout constants
  constexpr std::size_t kTrackHotHeaderSize = 36;
  constexpr std::size_t kTrackHotHeaderAlignment = 4;

  static_assert(sizeof(TrackHotHeader) == kTrackHotHeaderSize, "TrackHotHeader must be exactly 36 bytes");
  static_assert(alignof(TrackHotHeader) == kTrackHotHeaderAlignment, "TrackHotHeader must have 4-byte alignment");

  /** Storage representation for one typed cover-art resource reference. */
  struct CoverArtEntry final
  {
    ResourceId id{};     // 4 bytes - Links to the blob in ResourceStore
    std::uint8_t type{}; // 1 byte - PictureType enum value
    std::array<std::uint8_t, 3> reserved{};
  };

  static_assert(sizeof(CoverArtEntry) == 8, "CoverArtEntry must be exactly 8 bytes");
  static_assert(alignof(CoverArtEntry) == 4, "CoverArtEntry must have 4-byte alignment");

  enum class TrackColdBlockSlot : std::uint8_t
  {
    CoverArt = 0,
    Classical = 1,
    CustomMetadata = 2,
  };

  constexpr std::size_t kTrackColdKnownBlockSlotCount = 3;
  constexpr std::size_t kTrackColdBlockSlotCount = 5;

  constexpr std::size_t trackColdBlockSlotIndex(TrackColdBlockSlot slot) noexcept
  {
    return static_cast<std::size_t>(slot);
  }

  struct TrackClassicalBlock final
  {
    DictionaryId workId{};
    DictionaryId movementId{};
    DictionaryId conductorId{};
    DictionaryId ensembleId{};
    DictionaryId soloistId{};
    std::uint16_t movementNumber{};
    std::uint16_t movementTotal{};
  };

  static_assert(sizeof(TrackClassicalBlock) == 24, "TrackClassicalBlock must be exactly 24 bytes");
  static_assert(alignof(TrackClassicalBlock) == 4, "TrackClassicalBlock must have 4-byte alignment");

  struct CustomMetadataBlockHeader final
  {
    std::uint16_t entryCount{};
    std::uint16_t valueOffset{};
    std::uint16_t payloadLength{};
    std::uint16_t reserved{};
  };

  static_assert(sizeof(CustomMetadataBlockHeader) == 8, "CustomMetadataBlockHeader must be exactly 8 bytes");
  static_assert(alignof(CustomMetadataBlockHeader) <= 4, "CustomMetadataBlockHeader must fit cold record alignment");

  /**
   * CustomMetadataEntry - Binary entry for custom key-value metadata in the custom block payload.
   * 8 bytes total, 4-byte aligned.
   */
  struct CustomMetadataEntry final
  {
    DictionaryId keyId{};          // 4 bytes - Links to the key string in DictionaryStore
    std::uint16_t valueOffset = 0; // 2 bytes - byte offset from custom payload start to value
    std::uint16_t valueLength = 0; // 2 bytes - value length in bytes
  };

  static_assert(sizeof(CustomMetadataEntry) == 8, "CustomMetadataEntry must be exactly 8 bytes");
  static_assert(alignof(CustomMetadataEntry) == 4, "CustomMetadataEntry must have 4-byte alignment");

  /**
   * TrackColdHeader - POD struct for cold track fixed fields and extension block offsets.
   * Layout uses strictly descending member sizes (4→2→1) for natural alignment.
   *
   * Cold fixed fields are those not used in high-frequency filter/sort operations:
   *   - duration, bitrate, channels: audio properties
   *   - trackNumber, trackTotal, discNumber, discTotal: display only
   *   - extension block area: cover art, classical metadata, custom metadata
   *   - uri: playback path, stored after the block area
   *
   * Total size: 32 bytes with 4-byte alignment.
   *
   * Layout:
   *   ┌─────────────────────────────────────┐  ← cold data begin
   *   │        TrackColdHeader (32B)        │
   *   │  duration, bitrate,                 │
   *   │  trackNumber, trackTotal,           │
   *   │  discNumber, discTotal,             │
   *   │  blockOffsets[5], uriOffset,        │
   *   │  uriLength, channels, reserved8     │
   *   ├─────────────────────────────────────┤  ← first present block offset
   *   │  block payload                      │
   *   │  ...                                │
   *   ├─────────────────────────────────────┤  ← uriOffset
   *   │  uri data... (uriLength bytes)      │
   *   └─────────────────────────────────────┘  ← cold data end
   */
  struct TrackColdHeader final
  {
    // 4-byte section
    TrackDuration duration{}; // Track duration (millisecond span)
    Bitrate bitrate{};        // Bitrate in bps

    // 2-byte section
    std::uint16_t trackNumber{};                                        // Track number
    std::uint16_t trackTotal{};                                         // Total tracks in album
    std::uint16_t discNumber{};                                         // Disc number
    std::uint16_t discTotal{};                                          // Total discs in album
    std::array<std::uint16_t, kTrackColdBlockSlotCount> blockOffsets{}; // 0 means absent
    std::uint16_t uriOffset{};                                          // Byte offset from header start to uri string
    std::uint16_t uriLength{};                                          // Length of URI string

    // 1-byte section
    Channels channels{}; // Number of audio channels
    std::uint8_t reserved8{};
  };

  // Binary layout constants
  constexpr std::size_t kTrackColdHeaderSize = 32;
  constexpr std::size_t kTrackColdHeaderAlignment = 4;

  static_assert(sizeof(TrackColdHeader) == kTrackColdHeaderSize, "TrackColdHeader must be exactly 32 bytes");
  static_assert(alignof(TrackColdHeader) == kTrackColdHeaderAlignment, "TrackColdHeader must have 4-byte alignment");

  static_assert(sizeof(DictionaryId) == 4, "DictionaryId must stay 4 bytes");
  static_assert(alignof(DictionaryId) == 4, "DictionaryId must stay 4-byte aligned");
  static_assert(sizeof(ResourceId) == 4, "ResourceId must stay 4 bytes");
  static_assert(alignof(ResourceId) == 4, "ResourceId must stay 4-byte aligned");
  static_assert(sizeof(Bitrate) == 4, "Bitrate must stay 4 bytes");
  static_assert(alignof(Bitrate) == 4, "Bitrate must stay 4-byte aligned");
  static_assert(sizeof(Channels) == 1, "Channels must stay 1 byte");
  static_assert(alignof(Channels) == 1, "Channels must stay 1-byte aligned");
  static_assert(sizeof(TrackDuration) == 4, "TrackDuration must stay 4 bytes");
  static_assert(alignof(TrackDuration) == 4, "TrackDuration must stay 4-byte aligned");
} // namespace ao::library
