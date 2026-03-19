// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/DictionaryStore.h>
#include <rs/core/TrackLayout.h>

#include <string>
#include <string_view>
#include <vector>

namespace rs::core
{

  /**
   * TrackRecord - Materialized track data with resolved dictionary strings.
   *
   * Use proxy methods to access fields by category:
   *   - property()  : @duration, @bitrate, @sampleRate, @channels, @bitDepth
   *   - metadata()  : $artist, $album, $genre, $year, $title, $trackNumber
   *   - tags()      : #rock, #favorites, etc.
   */
  class TrackRecord
  {
  public:
    TrackRecord() = default;

    /**
     * Construct from a TrackView by resolving dictionary IDs.
     *
     * @param view The binary track view
     * @param dict DictionaryStore to resolve artist/album/genre IDs
     */
    TrackRecord(TrackView const& view, DictionaryStore const& dict);

    /**
     * Property - Audio file technical properties (@ prefix).
     */
    struct Property
    {
      std::uint64_t fileSize = 0;
      std::uint64_t mtime = 0;
      std::uint32_t durationMs = 0;
      std::uint32_t bitrate = 0;
      std::uint32_t sampleRate = 0;
      std::uint16_t codecId = 0;
      std::uint8_t channels = 0;
      std::uint8_t bitDepth = 0;
      std::uint8_t rating = 0;
    };

    /**
     * Metadata - Music information ($ prefix).
     */
    struct Metadata
    {
      std::string title;
      std::string uri;
      std::string artist;
      std::string album;
      std::string albumArtist;
      std::string genre;
      std::uint16_t year = 0;
      std::uint16_t trackNumber = 0;
      std::uint16_t totalTracks = 0;
      std::uint16_t discNumber = 0;
      std::uint16_t totalDiscs = 0;
      std::uint32_t coverArtId = 0; // ResourceStore ID for cover art
    };

    /**
     * Tags - User-defined labels (# prefix).
     */
    struct Tags
    {
      std::vector<DictionaryId> ids; // Tag IDs for serialization
    };

    Property property;
    Metadata metadata;
    Tags tags;

    // Dictionary IDs - resolved at creation time
    DictionaryId artistId;
    DictionaryId albumId;
    DictionaryId genreId;
    DictionaryId albumArtistId;

    /**
     * Serialize this record to binary format for LMDB storage.
     *
     * @return Vector of bytes suitable for TrackStore::create
     */
    std::vector<std::byte> serialize() const;

    /**
     * Get the header with current field values.
     */
    TrackHeader header() const;
  };

} // namespace rs::core
