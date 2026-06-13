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
   * Total size: 40 bytes with 4-byte alignment.
   *
   * Layout:
   *   ┌─────────────────────────────────────┐  ← hot data begin
   *   │        TrackHotHeader (40B)         |
   *   │  tagBloom, sampleRate,              │
   *   │  artistId, albumId,                 │
   *   │  genreId, albumArtistId,            │
   *   │  composerId, year,                  │
   *   │  bitDepth, titleLen, tagLen,        │
   *   │  codec, rating, padding             │
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
    std::uint32_t tagBloom{};     // Bloom filter for tags (32-bit)
    DictionaryId artistId{};      // Dictionary ID for artist
    DictionaryId albumId{};       // Dictionary ID for album
    DictionaryId genreId{};       // Dictionary ID for genre
    DictionaryId albumArtistId{}; // Dictionary ID for album artist
    DictionaryId composerId{};    // Dictionary ID for composer
    std::uint32_t sampleRate{};   // Sample rate in Hz

    // 2-byte section
    std::uint16_t year{};     // Release year
    std::uint16_t bitDepth{}; // Bits per sample
    std::uint16_t titleLen{}; // Length of title string
    std::uint16_t tagLen{};   // Length of tags blob in bytes

    // 1-byte section
    AudioCodec codec{};    // Audio codec
    std::uint8_t rating{}; // User rating (0-5)

    // 2 bytes padding to reach 40 bytes total
    std::array<std::byte, 2> padding{};
  };

  // Binary layout constants
  constexpr std::size_t kTrackHotHeaderSize = 40;
  constexpr std::size_t kTrackHotHeaderAlignment = 4;

  static_assert(sizeof(TrackHotHeader) == kTrackHotHeaderSize, "TrackHotHeader must be exactly 40 bytes");
  static_assert(alignof(TrackHotHeader) == kTrackHotHeaderAlignment, "TrackHotHeader must have 4-byte alignment");

  /**
   * TrackColdHeader - POD struct for cold track fixed fields.
   * Layout uses strictly descending member sizes (4→2→1) for natural alignment.
   *
   * Cold fixed fields are those not used in high-frequency filter/sort operations:
   *   - duration, bitrate, channels: audio properties
   *   - coverArtId: display only
   *   - trackNumber, totalTracks, discNumber, totalDiscs: display only
   *   - workId: classical metadata
   *   - uri: playback path, not filtered
   *
   * Total size: 32 bytes with 4-byte alignment.
   *
   * Layout:
   *   ┌─────────────────────────────────────┐  ← cold data begin
   *   │        TrackColdHeader (32B)        │
   *   │  duration, coverArtId, bitrate,     │
   *   │  workId                             │
   *   │  trackNumber, totalTracks,          │
   *   │  discNumber, totalDiscs             │
   *   │  customCount, uriOffset, uriLen     │
   *   │  channels, padding                  │
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
    // 4-byte section
    TrackDuration duration{};   // Track duration (millisecond span)
    std::uint32_t coverArtId{}; // ResourceStore ID for cover art
    std::uint32_t bitrate{};    // Bitrate in bps
    DictionaryId workId{};      // Dictionary ID for work

    // 2-byte section
    std::uint16_t trackNumber{}; // Track number
    std::uint16_t totalTracks{}; // Total tracks in album
    std::uint16_t discNumber{};  // Disc number
    std::uint16_t totalDiscs{};  // Total discs in album
    std::uint16_t customCount{}; // Number of custom key-value entries
    std::uint16_t uriOffset{};   // Byte offset from header start to uri string
    std::uint16_t uriLen{};      // Length of URI string

    // 1-byte section
    std::uint8_t channels{}; // Number of audio channels

    // 1 byte padding to reach 32 bytes total
    std::array<std::byte, 1> padding{};
  };

  // Binary layout constants
  constexpr std::size_t kTrackColdHeaderSize = 32;
  constexpr std::size_t kTrackColdHeaderAlignment = 4;

  static_assert(sizeof(TrackColdHeader) == kTrackColdHeaderSize, "TrackColdHeader must be exactly 32 bytes");
  static_assert(alignof(TrackColdHeader) == kTrackColdHeaderAlignment, "TrackColdHeader must have 4-byte alignment");
} // namespace ao::library
