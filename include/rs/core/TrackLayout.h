/*
 * Copyright (C) 2025 RockStudio
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
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

#include <cstdint>
#include <string_view>
#include <vector>

namespace rs::core
{

  /**
   * TrackHeader - POD struct for binary track storage.
   * Layout uses strictly descending member sizes (8→4→2→1) for natural alignment.
   * Total size: 72 bytes with 8-byte alignment.
   */
  struct TrackHeader
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

  /* Static assertions to guarantee layout - placed after struct definition */
  static_assert(sizeof(TrackHeader) == 72, "TrackHeader must be exactly 72 bytes");
  static_assert(alignof(TrackHeader) == 8, "TrackHeader must have 8-byte alignment");

  /**
   * TrackView - Safe accessor for track data stored in binary format.
   * Provides bounds-checked access to variable-length strings.
   *
   * Use proxy methods to access fields by category:
   *   - property()  : @duration, @bitrate, @sampleRate, @channels, @bitDepth
   *   - metadata()  : $artist, $album, $genre, $year, $title, $trackNumber
   *   - tags()      : #rock, #favorites, etc.
   */
  class TrackView
  {
    // Friend declarations for proxy classes
    friend class PropertyProxy;
    friend class MetadataProxy;
    friend class TagProxy;

  public:
    /**
     * PropertyProxy - Accessors for audio file technical properties (@ prefix).
     */
    class PropertyProxy
    {
    public:
      explicit PropertyProxy(const TrackView& track) : _h(*track.header()), _track(track) {}

      std::uint32_t durationMs() const noexcept { return _h.durationMs; }
      std::uint32_t bitrate() const noexcept { return _h.bitrate; }
      std::uint32_t sampleRate() const noexcept { return _h.sampleRate; }
      std::uint8_t channels() const noexcept { return _h.channels; }
      std::uint8_t bitDepth() const noexcept { return _h.bitDepth; }
      std::uint8_t rating() const noexcept { return _h.rating; }
      std::uint64_t fileSize() const noexcept { return _h.fileSize; }
      std::uint64_t mtime() const noexcept { return _h.mtime; }
      std::uint16_t codecId() const noexcept { return _h.codecId; }
      std::string_view uri() const { return _track.uri(); }

    private:
      const TrackHeader& _h;
      const TrackView& _track;
    };

    /**
     * MetadataProxy - Accessors for music metadata ($ prefix).
     */
    class MetadataProxy
    {
    public:
      explicit MetadataProxy(const TrackView& track) : _track(track) {}

      std::string_view title() const { return _track.title(); }
      DictionaryId artistId() const noexcept { return _track.header()->artistId; }
      DictionaryId albumId() const noexcept { return _track.header()->albumId; }
      DictionaryId genreId() const noexcept { return _track.header()->genreId; }
      DictionaryId albumArtistId() const noexcept { return _track.header()->albumArtistId; }
      std::uint32_t coverArtId() const noexcept { return _track.header()->coverArtId; }
      std::uint16_t year() const noexcept { return _track.header()->year; }
      std::uint16_t trackNumber() const noexcept { return _track.header()->trackNumber; }

    private:
      const TrackView& _track;
    };

    /**
     * TagProxy - Accessors for user-defined tags (# prefix).
     */
    class TagProxy
    {
    public:
      explicit TagProxy(const TrackView& track) : _track(track) {}

      std::uint8_t count() const noexcept { return _track.header()->tagCount; }
      std::uint32_t bloom() const noexcept { return _track.header()->tagBloom; }
      DictionaryId id(std::uint8_t index) const { return DictionaryId{_track.tagId(index)}; }
      std::vector<DictionaryId> ids() const;
      bool has(DictionaryId tagIdToCheck) const;

    private:
      const TrackView& _track;
    };

    TrackView() noexcept : _header(nullptr), _payloadBase(nullptr), _size(0) {}

    TrackView(const void* data, std::size_t size) noexcept
      : _header(static_cast<const TrackHeader*>(data))
      , _payloadBase(static_cast<const std::uint8_t*>(data))
      , _size(size)
    {
    }

    bool is_valid() const noexcept { return _header != nullptr && _size >= sizeof(TrackHeader); }

    // Proxy accessors for type-safe field access
    PropertyProxy property() const { return PropertyProxy(*this); }
    MetadataProxy metadata() const { return MetadataProxy(*this); }
    TagProxy tags() const { return TagProxy(*this); }

    // Essential accessors
    const TrackHeader* header() const noexcept { return _header; }

    // Get the raw payload data (everything after the header)
    std::string_view payload() const
    {
      if (!is_valid())
        return {};
      auto payloadStart = _payloadBase + sizeof(TrackHeader);
      auto payloadSize = _size - sizeof(TrackHeader);
      return {reinterpret_cast<const char*>(payloadStart), payloadSize};
    }

  private:
    const TrackHeader* _header;
    const std::uint8_t* _payloadBase;
    std::size_t _size;

    std::string_view getString(std::uint16_t offset, std::uint16_t len) const;

    // For proxy access
    std::string_view title() const { return getString(_header->titleOffset, _header->titleLen); }
    std::string_view uri() const { return getString(_header->uriOffset, _header->uriLen); }
    std::uint32_t tagId(std::uint8_t index) const;
  };

} // namespace rs::core
