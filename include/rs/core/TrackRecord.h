// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/TrackLayout.h>

#include <string>
#include <vector>

namespace rs::core
{

  /**
   * TrackRecord - Pure domain model for track data.
   *
   * All fields are strings (no DictionaryIds). Serialization and string
   * resolution happens in TrackBuilder.
   */
  class TrackRecord
  {
  public:
    TrackRecord() = default;

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
     * Stored as string names; resolved to DictionaryIds at TrackBuilder serialize time.
     */
    struct Tags
    {
      std::vector<std::string> names; // Tag names for serialization
    };

    /**
     * Custom - User-defined key-value pairs (stored in cold storage).
     */
    struct Custom
    {
      std::vector<std::pair<std::string, std::string>> pairs;
    };

    // Member variables
    Property property;
    Metadata metadata;
    Tags tags;
    Custom custom;
  };

} // namespace rs::core
