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

  /**
   * TrackHotHeader - POD struct for hot track storage.
   * Hot fields are used for fast filtering/sorting operations.
   *
   * Layout uses strictly descending member sizes (4→2→1) for natural alignment.
   * Total size: 36 bytes with 4-byte alignment.
   *
   * Byte layout:
   *   [0-3]    tagBloom (4)
   *   [4-7]    artistId (4)
   *   [8-11]   albumId (4)
   *   [12-15]  genreId (4)
   *   [16-19]  albumArtistId (4)
   *   [20-21]  year (2)
   *   [22-23]  codecId (2)
   *   [24-25]  bitDepth (2)
   *   [26-27]  titleOffset (2)
   *   [28-29]  titleLen (2)
   *   [30-31]  tagsOffset (2)
   *   [32]     rating (1)
   *   [33]     tagCount (1)
   *   [34-35]  padding (2)
   */
  struct TrackHotHeader final
  {
    // 4-byte section
    std::uint32_t tagBloom; // Bloom filter for tags (32-bit)
    DictionaryId artistId;      // Dictionary ID for artist
    DictionaryId albumId;       // Dictionary ID for album
    DictionaryId genreId;       // Dictionary ID for genre
    DictionaryId albumArtistId; // Dictionary ID for album artist

    // 2-byte section
    std::uint16_t year;        // Release year
    std::uint16_t codecId;     // Audio codec identifier
    std::uint16_t bitDepth;    // Bits per sample
    std::uint16_t titleOffset; // Offset to title string in payload
    std::uint16_t titleLen;    // Length of title string
    std::uint16_t tagsOffset;  // Offset to tags blob in payload

    // 1-byte section
    std::uint8_t rating;   // User rating (0-5)
    std::uint8_t tagCount; // Number of tags

    // 2 bytes padding to reach 36 bytes total
    std::uint8_t padding[2];
  };

  // Binary layout constants
  constexpr std::size_t kTrackHotHeaderSize = 36;
  constexpr std::size_t kTrackHotHeaderAlignment = 4;

  static_assert(sizeof(TrackHotHeader) == kTrackHotHeaderSize, "TrackHotHeader must be exactly 36 bytes");
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
   * Total size: 44 bytes with 4-byte alignment.
   *
   * Byte layout:
   *   [0-3]    fileSizeLo (4) - Lower 32 bits of file size
   *   [4-7]    fileSizeHi (4) - Upper 32 bits of file size
   *   [8-11]   mtimeLo (4)    - Lower 32 bits of modification time
   *   [12-15]  mtimeHi (4)    - Upper 32 bits of modification time
   *   [16-19]  durationMs (4)
   *   [20-23]  sampleRate (4)
   *   [24-27]  coverArtId (4)
   *   [28-31]  bitrate (4)
   *   [32-33]  trackNumber (2)
   *   [34-35]  totalTracks (2)
   *   [36-37]  discNumber (2)
   *   [38-39]  totalDiscs (2)
   *   [40-41]  uriOffset (2)
   *   [42-43]  uriLen (2)
   *   [44]     channels (1)
   *   [45-47]  padding (3)
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
    std::uint32_t bitrate;     // Bitrate in bps

    // 2-byte section
    std::uint16_t trackNumber;  // Track number
    std::uint16_t totalTracks; // Total tracks in album
    std::uint16_t discNumber;   // Disc number
    std::uint16_t totalDiscs;  // Total discs in album
    std::uint16_t uriOffset;    // Offset to URI string in payload
    std::uint16_t uriLen;       // Length of URI string

    // 1-byte section
    std::uint8_t channels; // Number of audio channels

    // 3 bytes padding to reach 48 bytes total
    std::uint8_t padding[3];
  };

  // Binary layout constants
  constexpr std::size_t kTrackColdHeaderSize = 48;
  constexpr std::size_t kTrackColdHeaderAlignment = 4;

  static_assert(sizeof(TrackColdHeader) == kTrackColdHeaderSize, "TrackColdHeader must be exactly 48 bytes");
  static_assert(alignof(TrackColdHeader) == kTrackColdHeaderAlignment, "TrackColdHeader must have 4-byte alignment");

  /**
   * Encode cold track data: fixed header + custom KV + uri.
   */
  std::vector<std::byte> encodeColdData(TrackColdHeader const& header,
                                        std::vector<std::pair<std::string, std::string>> const& customMeta,
                                        std::string_view uri);

  /**
   * Encode just the custom key-value pairs into cold binary format.
   */
  std::vector<std::byte> encodeColdCustomMeta(std::vector<std::pair<std::string, std::string>> const& customMeta);

  /**
   * Normalize a custom key: lowercase + trim whitespace.
   * Used during ingestion to ensure consistent key lookup.
   */
  std::string normalizeKey(std::string_view key);

} // namespace rs::core