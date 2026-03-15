/*
 * Copyright (C) 2025 RockStudio
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <rs/core/Dictionary.h>
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
     * @param dict Dictionary to resolve artist/album/genre IDs
     * @param txn Transaction for dictionary lookups
     */
    TrackRecord(TrackView const& view, Dictionary const& dict, lmdb::ReadTransaction& txn);

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

    /**
     * Serialize this record to binary format for LMDB storage.
     *
     * @return Vector of bytes suitable for TrackStore::create
     */
    [[nodiscard]] std::vector<std::byte> serialize() const;

    /**
     * Get the header with current field values.
     */
    [[nodiscard]] TrackHeader header() const;
  };

} // namespace rs::core
