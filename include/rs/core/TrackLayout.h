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
   * Layout uses strictly descending member sizes (8→4→2→1) for natural alignment.
   * Total size: 80 bytes with 8-byte alignment.
   */
  struct TrackHotHeader final
  {
    // 8-byte section
    std::uint64_t fileSize; // File size in bytes
    std::uint64_t mtime;    // Last modification time (unix timestamp)

    // 4-byte section
    std::uint32_t tagBloom;     // Bloom filter for tags (32-bit)
    std::uint32_t durationMs;   // Track duration in milliseconds
    std::uint32_t bitrate;      // Bitrate in bps
    std::uint32_t sampleRate;   // Sample rate in Hz
    DictionaryId artistId;      // Dictionary ID for artist
    DictionaryId albumId;       // Dictionary ID for album
    DictionaryId genreId;       // Dictionary ID for genre
    DictionaryId albumArtistId; // Dictionary ID for album artist
    std::uint32_t coverArtId;   // ResourceStore ID for cover art

    // 2-byte section
    std::uint16_t year;        // Release year
    std::uint16_t trackNumber; // Track number
    std::uint16_t totalTracks; // Total tracks in album
    std::uint16_t discNumber;  // Disc number
    std::uint16_t totalDiscs;  // Total discs in album
    std::uint16_t codecId;     // Audio codec identifier
    std::uint16_t titleOffset; // Offset to title string in payload
    std::uint16_t titleLen;    // Length of title string
    std::uint16_t uriOffset;   // Offset to URI string in payload
    std::uint16_t uriLen;      // Length of URI string
    std::uint16_t tagsOffset;  // Offset to tags blob in payload

    // 1-byte section
    std::uint8_t channels; // Number of audio channels
    std::uint8_t bitDepth; // Bits per sample
    std::uint8_t rating;   // User rating (0-5)
    std::uint8_t tagCount; // Number of tags
  };

  // Binary layout constants
  constexpr std::size_t kTrackHotHeaderSize = 80;    // 76 bytes + 4 padding for 8-byte alignment
  constexpr std::size_t kTrackHotHeaderAlignment = 8;

  static_assert(sizeof(TrackHotHeader) == kTrackHotHeaderSize, "TrackHotHeader must be exactly 80 bytes");
  static_assert(alignof(TrackHotHeader) == kTrackHotHeaderAlignment, "TrackHotHeader must have 8-byte alignment");

  /**
   * TrackColdHeader - POD struct for cold track fixed fields.
   * Layout uses strictly descending member sizes (8→4→2→1) for natural alignment.
   *
   * Cold fixed fields are those not used in high-frequency filter/sort operations:
   *   - fileSize, mtime: file identity / refresh detection
   *   - coverArtId: display only
   *   - trackNumber, totalTracks, discNumber, totalDiscs: display only
   *   - uri: playback path, not filtered
   *
   * Total size: 40 bytes with 8-byte alignment.
   */
  struct TrackColdHeader final
  {
    // 8-byte section
    std::uint64_t fileSize; // File size in bytes
    std::uint64_t mtime;    // Last modification time (unix timestamp)

    // 4-byte section
    std::uint32_t coverArtId; // ResourceStore ID for cover art

    // 2-byte section
    std::uint16_t trackNumber; // Track number
    std::uint16_t totalTracks; // Total tracks in album
    std::uint16_t discNumber;  // Disc number
    std::uint16_t totalDiscs;  // Total discs in album
    std::uint16_t uriOffset;   // Offset to URI string in payload
    std::uint16_t uriLen;      // Length of URI string

    // 1-byte section (padding to 8-byte alignment)
    std::uint8_t reserved; // Padding for alignment
  };

  // Binary layout constants
  constexpr std::size_t kTrackColdHeaderSize = 40; // 33 bytes + 7 padding for 8-byte alignment
  constexpr std::size_t kTrackColdHeaderAlignment = 8;

  static_assert(sizeof(TrackColdHeader) == kTrackColdHeaderSize, "TrackColdHeader must be exactly 40 bytes");
  static_assert(alignof(TrackColdHeader) == kTrackColdHeaderAlignment, "TrackColdHeader must have 8-byte alignment");

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