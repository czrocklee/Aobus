// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/DictionaryStore.h>
#include <rs/core/TrackView.h>

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
     * Construct from unified TrackView by resolving dictionary IDs.
     *
     * @param view The unified track view (both hot and cold must be valid)
     * @param dict DictionaryStore to resolve artist/album/genre IDs
     * @throws rs::Exception if view.isHotValid() or view.isColdValid() is false
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

    /**
     * Custom - User-defined key-value pairs (stored in cold storage).
     */
    struct Custom
    {
      std::vector<std::pair<std::string, std::string>> pairs;
    };

    Property property;
    Metadata metadata;
    Tags tags;
    Custom custom;

    // Dictionary IDs - resolved at creation time
    DictionaryId artistId;
    DictionaryId albumId;
    DictionaryId genreId;
    DictionaryId albumArtistId;

    /**
     * Serialize hot fields to binary format for tracks_hot DB.
     *
     * @return Vector of bytes suitable for TrackStore::Writer::createHotCold
     */
    std::vector<std::byte> serializeHot() const;

    /**
     * Serialize cold fields to binary format for tracks_cold DB.
     *
     * Custom keys are resolved to DictionaryIds via the provided DictionaryStore.
     * The indexed format stores entries sorted by dictId for O(log n) binary search.
     *
     * @param dict DictionaryStore to resolve custom key strings to DictionaryIds
     * @return Vector of bytes suitable for TrackStore::Writer::createHotCold
     */
    std::vector<std::byte> serializeCold(DictionaryStore const& dict) const;

    /**
     * Serialize cold fields using a custom key resolver function.
     * Convenience overload for testing without a full DictionaryStore.
     *
     * @param resolveKey Function that maps string key to DictionaryId
     * @return Vector of bytes suitable for TrackStore::Writer::createHotCold
     */
    std::vector<std::byte> serializeCold(std::function<DictionaryId(std::string_view)> const& resolveKey) const;

    /**
     * Get the hot header with current field values.
     */
    TrackHotHeader hotHeader() const;

    /**
     * Get the cold header with current field values.
     */
    TrackColdHeader coldHeader() const;
  };

} // namespace rs::core
