// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

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
  struct TrackHeader final
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
  constexpr std::size_t kTrackHeaderSize = 80;
  constexpr std::size_t kTrackHeaderAlignment = 8;

  /* Static assertions to guarantee layout - placed after struct definition */
  static_assert(sizeof(TrackHeader) == kTrackHeaderSize, "TrackHeader must be exactly 80 bytes");
  static_assert(alignof(TrackHeader) == kTrackHeaderAlignment, "TrackHeader must have 8-byte alignment");

  /**
   * TrackView - Safe accessor for track data stored in binary format.
   * Provides bounds-checked access to variable-length strings.
   *
   * Use proxy methods to access fields by category:
   *   - property()  : @duration, @bitrate, @sampleRate, @channels, @bitDepth
   *   - metadata()  : $artist, $album, $genre, $year, $title, $trackNumber
   *   - tags()      : #rock, #favorites, etc.
   */
  class TrackView final
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
      explicit PropertyProxy(TrackView const& track) : _h(*track.header()), _track(track) {}

      [[nodiscard]] std::uint32_t durationMs() const noexcept { return _h.durationMs; }
      [[nodiscard]] std::uint32_t bitrate() const noexcept { return _h.bitrate; }
      [[nodiscard]] std::uint32_t sampleRate() const noexcept { return _h.sampleRate; }
      [[nodiscard]] std::uint8_t channels() const noexcept { return _h.channels; }
      [[nodiscard]] std::uint8_t bitDepth() const noexcept { return _h.bitDepth; }
      [[nodiscard]] std::uint8_t rating() const noexcept { return _h.rating; }
      [[nodiscard]] std::uint64_t fileSize() const noexcept { return _h.fileSize; }
      [[nodiscard]] std::uint64_t mtime() const noexcept { return _h.mtime; }
      [[nodiscard]] std::uint16_t codecId() const noexcept { return _h.codecId; }
      [[nodiscard]] std::string_view uri() const { return _track.uri(); }

    private:
      TrackHeader const& _h;
      TrackView const& _track;
    };

    /**
     * MetadataProxy - Accessors for music metadata ($ prefix).
     */
    class MetadataProxy
    {
    public:
      explicit MetadataProxy(TrackView const& track) : _track(track) {}

      [[nodiscard]] std::string_view title() const { return _track.title(); }
      [[nodiscard]] DictionaryId artistId() const noexcept { return _track.header()->artistId; }
      [[nodiscard]] DictionaryId albumId() const noexcept { return _track.header()->albumId; }
      [[nodiscard]] DictionaryId genreId() const noexcept { return _track.header()->genreId; }
      [[nodiscard]] DictionaryId albumArtistId() const noexcept { return _track.header()->albumArtistId; }
      [[nodiscard]] std::uint32_t coverArtId() const noexcept { return _track.header()->coverArtId; }
      [[nodiscard]] std::uint16_t year() const noexcept { return _track.header()->year; }
      [[nodiscard]] std::uint16_t trackNumber() const noexcept { return _track.header()->trackNumber; }
      [[nodiscard]] std::uint16_t totalTracks() const noexcept { return _track.header()->totalTracks; }
      [[nodiscard]] std::uint16_t discNumber() const noexcept { return _track.header()->discNumber; }
      [[nodiscard]] std::uint16_t totalDiscs() const noexcept { return _track.header()->totalDiscs; }

    private:
      TrackView const& _track;
    };

    /**
     * TagProxy - Accessors for user-defined tags (# prefix).
     */
    class TagProxy
    {
    public:
      explicit TagProxy(TrackView const& track) : _track(track) {}

      [[nodiscard]] std::uint8_t count() const noexcept { return _track.header()->tagCount; }
      [[nodiscard]] std::uint32_t bloom() const noexcept { return _track.header()->tagBloom; }
      [[nodiscard]] DictionaryId id(std::uint8_t index) const { return DictionaryId{_track.tagId(index)}; }
      [[nodiscard]] std::vector<DictionaryId> ids() const;
      [[nodiscard]] bool has(DictionaryId tagIdToCheck) const;

    private:
      TrackView const& _track;
    };

    TrackView() noexcept : _header(nullptr), _payloadBase(nullptr), _size(0) {}

    TrackView(void const* data, std::size_t size) noexcept
      : _header(static_cast<TrackHeader const*>(data))
      , _payloadBase(static_cast<std::uint8_t const*>(data))
      , _size(size)
    {
    }

    [[nodiscard]] bool isValid() const noexcept { return _header != nullptr && _size >= sizeof(TrackHeader); }

    // Proxy accessors for type-safe field access
    [[nodiscard]] PropertyProxy property() const { return PropertyProxy(*this); }
    [[nodiscard]] MetadataProxy metadata() const { return MetadataProxy(*this); }
    [[nodiscard]] TagProxy tags() const { return TagProxy(*this); }

    // Essential accessors
    [[nodiscard]] TrackHeader const* header() const noexcept { return _header; }

    // Get the raw payload data (everything after the header)
    [[nodiscard]] std::string_view payload() const
    {
      if (!isValid()) return {};
      auto payloadStart = _payloadBase + sizeof(TrackHeader);
      auto payloadSize = _size - sizeof(TrackHeader);
      return {reinterpret_cast<char const*>(payloadStart), payloadSize};
    }

  private:
    TrackHeader const* _header;
    std::uint8_t const* _payloadBase;
    std::size_t _size;

    [[nodiscard]] std::string_view getString(std::uint16_t offset, std::uint16_t len) const;

    // For proxy access
    [[nodiscard]] std::string_view title() const { return getString(_header->titleOffset, _header->titleLen); }
    [[nodiscard]] std::string_view uri() const { return getString(_header->uriOffset, _header->uriLen); }
    [[nodiscard]] std::uint32_t tagId(std::uint8_t index) const;
  };

} // namespace rs::core
