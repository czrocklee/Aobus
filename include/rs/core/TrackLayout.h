// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/Exception.h>
#include <rs/core/DictionaryStore.h>

#include <cstdint>
#include <rs/utility/ByteView.h>
#include <span>
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
      explicit PropertyProxy(TrackView const& track) : _track(track) {}

      std::uint32_t durationMs() const noexcept { return _track.header()->durationMs; }
      std::uint32_t bitrate() const noexcept { return _track.header()->bitrate; }
      std::uint32_t sampleRate() const noexcept { return _track.header()->sampleRate; }
      std::uint8_t channels() const noexcept { return _track.header()->channels; }
      std::uint8_t bitDepth() const noexcept { return _track.header()->bitDepth; }
      std::uint8_t rating() const noexcept { return _track.header()->rating; }
      std::uint64_t fileSize() const noexcept { return _track.header()->fileSize; }
      std::uint64_t mtime() const noexcept { return _track.header()->mtime; }
      std::uint16_t codecId() const noexcept { return _track.header()->codecId; }
      std::string_view uri() const { return _track.uri(); }

    private:
      TrackView const& _track;
    };

    /**
     * MetadataProxy - Accessors for music metadata ($ prefix).
     */
    class MetadataProxy
    {
    public:
      explicit MetadataProxy(TrackView const& track) : _track(track) {}

      std::string_view title() const { return _track.title(); }
      DictionaryId artistId() const noexcept { return _track.header()->artistId; }
      DictionaryId albumId() const noexcept { return _track.header()->albumId; }
      DictionaryId genreId() const noexcept { return _track.header()->genreId; }
      DictionaryId albumArtistId() const noexcept { return _track.header()->albumArtistId; }
      std::uint32_t coverArtId() const noexcept { return _track.header()->coverArtId; }
      std::uint16_t year() const noexcept { return _track.header()->year; }
      std::uint16_t trackNumber() const noexcept { return _track.header()->trackNumber; }
      std::uint16_t totalTracks() const noexcept { return _track.header()->totalTracks; }
      std::uint16_t discNumber() const noexcept { return _track.header()->discNumber; }
      std::uint16_t totalDiscs() const noexcept { return _track.header()->totalDiscs; }

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

      std::uint8_t count() const noexcept { return _track.header()->tagCount; }
      std::uint32_t bloom() const noexcept { return _track.header()->tagBloom; }
      DictionaryId id(std::uint8_t index) const { return DictionaryId{_track.tagId(index)}; }
      std::vector<DictionaryId> ids() const;
      bool has(DictionaryId tagIdToCheck) const;

    private:
      TrackView const& _track;
    };

    TrackView() noexcept : _header{nullptr}, _size{0} {}

    explicit TrackView(std::span<std::byte const> data)
      : _header{utility::as<TrackHeader>(data)}
      , _size{data.size()}
    {
      if (data.size() < sizeof(TrackHeader)) { RS_THROW(rs::Exception, "Invalid track data: size too small"); }
    }

    bool isValid() const noexcept { return _header != nullptr && _size >= sizeof(TrackHeader); }

    // Proxy accessors for type-safe field access
    PropertyProxy property() const { return PropertyProxy{*this}; }
    MetadataProxy metadata() const { return MetadataProxy{*this}; }
    TagProxy tags() const { return TagProxy{*this}; }

    // Essential accessors
    TrackHeader const* header() const noexcept { return _header; }

  private:
    TrackHeader const* _header;
    std::size_t _size;

    std::string_view getString(std::uint16_t offset, std::uint16_t len) const;

    // For proxy access
    std::string_view title() const { return getString(_header->titleOffset, _header->titleLen); }
    std::string_view uri() const { return getString(_header->uriOffset, _header->uriLen); }
    std::uint32_t tagId(std::uint8_t index) const;
  };

} // namespace rs::core
