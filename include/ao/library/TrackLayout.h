// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Type.h>
#include <ao/library/AudioCodec.h>

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

  /**
   * CustomMetadataEntry - Binary entry for custom key-value metadata in TrackColdHeader.
   * 8 bytes total, 4-byte aligned.
   */
  struct CustomMetadataEntry final
  {
    DictionaryId keyId{};          // 4 bytes - Links to the key string in DictionaryStore
    std::uint16_t valueOffset = 0; // 2 bytes - byte offset from header start to value
    std::uint16_t valueLength = 0; // 2 bytes - value length in bytes
  };

  static_assert(sizeof(CustomMetadataEntry) == 8, "CustomMetadataEntry must be exactly 8 bytes");
  static_assert(alignof(CustomMetadataEntry) == 4, "CustomMetadataEntry must have 4-byte alignment");

  /**
   * TrackColdHeader - POD struct for cold track fixed fields.
   * Layout uses strictly descending member sizes (4→2→1) for natural alignment.
   *
   * Cold fixed fields are those not used in high-frequency filter/sort operations:
   *   - duration, bitrate, channels: audio properties
   *   - trackNumber, trackTotal, discNumber, discTotal: display only
   *   - workId, movementId, movementNumber, movementTotal: classical metadata
   *   - uri: playback path, not filtered
   *   - covers: ordered list of typed cover art ResourceStore references
   *
   * Total size: 40 bytes with 4-byte alignment.
   *
   * Cover entries are 8 bytes each: [resourceId(4), type(1), reserved(3)].
   * The primary cover is the first front cover, or entry 0 when no front cover exists.
   *
   * Layout:
   *   ┌─────────────────────────────────────┐  ← cold data begin
   *   │        TrackColdHeader (40B)        │
   *   │  duration, bitrate,                 │
   *   │  workId, movementId                 │
   *   │  trackNumber, trackTotal,           │
   *   │  discNumber, discTotal              │
   *   │  movementNumber, movementTotal      │
   *   │  customCount, uriOffset, uriLength  │
   *   │  coverCount, customOffset           │
   *   │  channels, padding                  │
   *   ├─────────────────────────────────────┤  ← coverCount * 8 bytes
   *   │  [id(4), type(1), rsv(3)] × M       │
   *   ├─────────────────────────────────────┤  ← customOffset
   *   │  [keyId(4), off(2), len(2)] × N     │
   *   ├─────────────────────────────────────┤  ← values start
   *   │  value 1                            │
   *   │  value 2                            │
   *   │  ...                                │
   *   ├─────────────────────────────────────┤  ← uri starts at uriOffset
   *   │  uri data... (uriLength bytes)      │
   *   └─────────────────────────────────────┘  ← cold data end
   */
  struct TrackColdHeader final
  {
    // 4-byte section
    TrackDuration duration{};  // Track duration (millisecond span)
    Bitrate bitrate{};         // Bitrate in bps
    DictionaryId workId{};     // Dictionary ID for work
    DictionaryId movementId{}; // Dictionary ID for movement name

    // 2-byte section
    std::uint16_t trackNumber{};    // Track number
    std::uint16_t trackTotal{};     // Total tracks in album
    std::uint16_t discNumber{};     // Disc number
    std::uint16_t discTotal{};      // Total discs in album
    std::uint16_t movementNumber{}; // Movement ordinal within the work
    std::uint16_t movementTotal{};  // Total movements in the work
    std::uint16_t customCount{};    // Number of custom key-value entries
    std::uint16_t uriOffset{};      // Byte offset from header start to uri string
    std::uint16_t uriLength{};      // Length of URI string
    std::uint16_t coverCount{};     // Number of cover art entries
    std::uint16_t customOffset{};   // Byte offset from header start to custom table

    // 1-byte section
    Channels channels{}; // Number of audio channels

    // 1 byte padding to reach 40 bytes total
    std::array<std::byte, 1> padding{};
  };

  // Binary layout constants
  constexpr std::size_t kTrackColdHeaderSize = 40;
  constexpr std::size_t kTrackColdHeaderAlignment = 4;

  static_assert(sizeof(TrackColdHeader) == kTrackColdHeaderSize, "TrackColdHeader must be exactly 40 bytes");
  static_assert(alignof(TrackColdHeader) == kTrackColdHeaderAlignment, "TrackColdHeader must have 4-byte alignment");
} // namespace ao::library
