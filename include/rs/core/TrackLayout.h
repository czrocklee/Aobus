// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/DictionaryStore.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace rs::core
{
  constexpr std::uint16_t kTrackLayoutVersion = 1;

  /**
   * TrackHotHeader - POD struct for hot track storage.
   * Hot fields are used for fast filtering/sorting operations.
   *
   * Layout uses strictly descending member sizes (4→2→1) for natural alignment.
   * Total size: 32 bytes with 4-byte alignment.
   *
   * Layout:
   *   ┌─────────────────────────────────────┐  ← hot data begin
   *   │        TrackHotHeader (32B)         |
   *   │  tagBloom, artistId, albumId,       │
   *   │  genreId, albumArtistId             │
   *   │  year, codecId, bitDepth            │
   *   │  titleLen, tagLen, rating           │
   *   │  padding                            │
   *   ├─────────────────────────────────────┤  ← tags begin = header + sizeof(header)
   *   │  tag ID 1 (4B)                      │
   *   │  tag ID 2 (4B)                      │
   *   │  ... (tagLen bytes total)           │
   *   ├─────────────────────────────────────┤  ← title begin = tags begin + tagLen
   *   │  title... (titleLen bytes)          │
   *   │  '\0'                               │
   *   └─────────────────────────────────────┘  ← hot data end
   */
  struct TrackHotHeader final
  {
    // 4-byte section
    std::uint32_t tagBloom;     // Bloom filter for tags (32-bit)
    DictionaryId artistId;      // Dictionary ID for artist
    DictionaryId albumId;       // Dictionary ID for album
    DictionaryId genreId;       // Dictionary ID for genre
    DictionaryId albumArtistId; // Dictionary ID for album artist

    // 2-byte section
    std::uint16_t year;     // Release year
    std::uint16_t codecId;  // Audio codec identifier
    std::uint16_t bitDepth; // Bits per sample
    std::uint16_t titleLen; // Length of title string
    std::uint16_t tagLen;   // Length of tags blob in bytes

    // 1-byte section
    std::uint8_t rating; // User rating (0-5)

    // 1 byte padding to reach 32 bytes total
    std::byte padding;
  };

  // Binary layout constants
  constexpr std::size_t kTrackHotHeaderSize = 32;
  constexpr std::size_t kTrackHotHeaderAlignment = 4;

  static_assert(sizeof(TrackHotHeader) == kTrackHotHeaderSize, "TrackHotHeader must be exactly 32 bytes");
  static_assert(alignof(TrackHotHeader) == kTrackHotHeaderAlignment, "TrackHotHeader must have 4-byte alignment");

  /**
   * TrackColdHeader - POD struct for cold track fixed fields.
   * Layout uses strictly descending member sizes (4→2→1) for natural alignment.
   *
   * NOTE: fileSize and mtime are split into two uint32_t fields to achieve
   * 4-byte alignment (LMDB's alignment guarantee). The original uint64_t values
   * are reconstructed in TrackView accessors.
   *
   * Cold fixed fields are those not used in high-frequency filter/sort operations:
   *   - fileSize, mtime: file identity / refresh detection
   *   - durationMs, bitrate, sampleRate, channels: audio properties
   *   - coverArtId: display only
   *   - trackNumber, totalTracks, discNumber, totalDiscs: display only
   *   - uri: playback path, not filtered
   *
   * Total size: 48 bytes with 4-byte alignment.
   *
   *
   * Layout:
   *   ┌─────────────────────────────────────┐  ← cold data begin
   *   │        TrackColdHeader (48B)        │
   *   │  fileSizeLo/Hi, mtimeLo/Hi          │
   *   │  durationMs, sampleRate,            │
   *   │  coverArtId, bitrate                │
   *   │  trackNumber, totalTracks,          │
   *   │  discNumber, totalDiscs             │
   *   │  customCount, uriOffset, uriLen     │
   *   │  channels, padding[3]               │
   *   ├─────────────────────────────────────┤  ← entries = customCount * 8 bytes
   *   │  [dictId(4), off(2), len(2)] × N   │
   *   ├─────────────────────────────────────┤  ← values start
   *   │  value 1                           │
   *   │  value 2                           │
   *   │  ...                               │
   *   ├─────────────────────────────────────┤  ← uri starts at uriOffset
   *   │  uri data... (uriLen bytes)         │
   *   └─────────────────────────────────────┘  ← cold data end
   */
  struct TrackColdHeader final
  {
    // 4-byte section (split from original int64)
    std::uint32_t fileSizeLo; // Lower 32 bits of file size
    std::uint32_t fileSizeHi; // Upper 32 bits of file size
    std::uint32_t mtimeLo;    // Lower 32 bits of modification time
    std::uint32_t mtimeHi;    // Upper 32 bits of modification time

    // 4-byte section
    std::uint32_t durationMs; // Track duration in milliseconds
    std::uint32_t sampleRate; // Sample rate in Hz
    std::uint32_t coverArtId; // ResourceStore ID for cover art
    std::uint32_t bitrate;    // Bitrate in bps

    // 2-byte section
    std::uint16_t trackNumber; // Track number
    std::uint16_t totalTracks; // Total tracks in album
    std::uint16_t discNumber;  // Disc number
    std::uint16_t totalDiscs;  // Total discs in album
    std::uint16_t customCount; // Number of custom key-value entries
    std::uint16_t uriOffset;   // Byte offset from header start to uri string
    std::uint16_t uriLen;      // Length of URI string

    // 1-byte section
    std::uint8_t channels; // Number of audio channels

    // 1 byte padding to reach 48 bytes total
    std::byte padding;
  };

  // Binary layout constants
  constexpr std::size_t kTrackColdHeaderSize = 48;
  constexpr std::size_t kTrackColdHeaderAlignment = 4;

  static_assert(sizeof(TrackColdHeader) == kTrackColdHeaderSize, "TrackColdHeader must be exactly 48 bytes");
  static_assert(alignof(TrackColdHeader) == kTrackColdHeaderAlignment, "TrackColdHeader must have 4-byte alignment");

} // namespace rs::core
