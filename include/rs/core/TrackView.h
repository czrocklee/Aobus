// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/TrackLayout.h>
#include <rs/Exception.h>

#include <cstdint>
#include <optional>
#include <rs/utility/ByteView.h>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace rs::core
{

  /**
   * TrackHotView - Safe accessor for hot track data stored in binary format.
   * Uses TrackHotHeader for the binary layout.
   * Provides bounds-checked access to variable-length strings.
   */
  class TrackHotView final
  {
    // Friend declarations for proxy classes
    friend class HotPropertyProxy;
    friend class HotMetadataProxy;
    friend class HotTagProxy;

  public:
    class HotPropertyProxy
    {
    public:
      explicit HotPropertyProxy(TrackHotView const& track) : _track(track) {}

      std::uint32_t durationMs() const noexcept { return _track.header()->durationMs; }
      std::uint32_t bitrate() const noexcept { return _track.header()->bitrate; }
      std::uint32_t sampleRate() const noexcept { return _track.header()->sampleRate; }
      std::uint8_t channels() const noexcept { return _track.header()->channels; }
      std::uint8_t bitDepth() const noexcept { return _track.header()->bitDepth; }
      std::uint8_t rating() const noexcept { return _track.header()->rating; }
      std::uint64_t fileSize() const noexcept { return _track.header()->fileSize; }
      std::uint64_t mtime() const noexcept { return _track.header()->mtime; }
      std::uint16_t codecId() const noexcept { return _track.header()->codecId; }

    private:
      TrackHotView const& _track;
    };

    class HotMetadataProxy
    {
    public:
      explicit HotMetadataProxy(TrackHotView const& track) : _track(track) {}

      std::string_view title() const { return _track.title(); }
      DictionaryId artistId() const noexcept { return _track.header()->artistId; }
      DictionaryId albumId() const noexcept { return _track.header()->albumId; }
      DictionaryId genreId() const noexcept { return _track.header()->genreId; }
      DictionaryId albumArtistId() const noexcept { return _track.header()->albumArtistId; }
      std::uint16_t year() const noexcept { return _track.header()->year; }

    private:
      TrackHotView const& _track;
    };

    class HotTagProxy
    {
    public:
      explicit HotTagProxy(TrackHotView const& track) : _track(track) {}

      std::uint8_t count() const noexcept { return _track.header()->tagCount; }
      std::uint32_t bloom() const noexcept { return _track.header()->tagBloom; }
      DictionaryId id(std::uint8_t index) const { return DictionaryId{_track.tagId(index)}; }
      std::vector<DictionaryId> ids() const;
      bool has(DictionaryId tagIdToCheck) const;

    private:
      TrackHotView const& _track;
    };

    TrackHotView() noexcept : _header{nullptr}, _size{0} {}

    explicit TrackHotView(std::span<std::byte const> data)
      : _header{utility::as<TrackHotHeader>(data)}
      , _size{data.size()}
    {
      if (data.size() < sizeof(TrackHotHeader)) { RS_THROW(rs::Exception, "Invalid hot track data: size too small"); }
    }

    bool isValid() const noexcept { return _header != nullptr && _size >= sizeof(TrackHotHeader); }

    HotPropertyProxy property() const { return HotPropertyProxy{*this}; }
    HotMetadataProxy metadata() const { return HotMetadataProxy{*this}; }
    HotTagProxy tags() const { return HotTagProxy{*this}; }

    TrackHotHeader const* header() const noexcept { return _header; }

  private:
    TrackHotHeader const* _header;
    std::size_t _size;

    std::string_view getString(std::uint16_t offset, std::uint16_t len) const;

    std::string_view title() const { return getString(_header->titleOffset, _header->titleLen); }
    std::uint32_t tagId(std::uint8_t index) const;
  };

  /**
   * TrackColdView - Binary layout for cold track data.
   *
   * Cold payload structure:
   *   TrackColdHeader (fixed fields, 40 bytes)
   *   Variable-length payload:
   *     uint16_t customPairCount
   *     Repeated custom entries:
   *       uint16_t keyLen
   *       uint16_t valueLen
   *       char[keyLen] key bytes (UTF-8)
   *       char[valueLen] value bytes (UTF-8)
   *     char[uriLen] uri bytes (null-terminated)
   *
   * Keys and values are UTF-8 encoded strings stored as-is (case preserved).
   */
  class TrackColdView final
  {
  public:
    /**
     * PropertyProxy - Accessors for cold track properties.
     */
    class PropertyProxy
    {
    public:
      explicit PropertyProxy(TrackColdView const& track) : _track(track) {}

      std::uint64_t fileSize() const noexcept { return _track.header()->fileSize; }
      std::uint64_t mtime() const noexcept { return _track.header()->mtime; }
      std::uint32_t coverArtId() const noexcept { return _track.header()->coverArtId; }
      std::uint16_t trackNumber() const noexcept { return _track.header()->trackNumber; }
      std::uint16_t totalTracks() const noexcept { return _track.header()->totalTracks; }
      std::uint16_t discNumber() const noexcept { return _track.header()->discNumber; }
      std::uint16_t totalDiscs() const noexcept { return _track.header()->totalDiscs; }
      std::string_view uri() const { return _track.uri(); }

    private:
      TrackColdView const& _track;
    };

    TrackColdView() noexcept : _header{nullptr}, _size{0} {}

    explicit TrackColdView(std::span<std::byte const> data)
      : _header{utility::as<TrackColdHeader>(data)}
      , _size{data.size()}
    {
      if (data.size() < sizeof(TrackColdHeader)) { RS_THROW(rs::Exception, "Invalid cold track data: size too small"); }
    }

    bool isNull() const noexcept { return _size == 0; }
    bool isEmpty() const noexcept { return _size == 0; }
    bool isValid() const noexcept { return _header != nullptr && _size >= sizeof(TrackColdHeader); }
    std::size_t size() const noexcept { return _size; }

    // Proxy accessor for type-safe field access
    PropertyProxy property() const { return PropertyProxy{*this}; }

    // Fixed field accessors (direct)
    std::uint64_t fileSize() const noexcept { return _header->fileSize; }
    std::uint64_t mtime() const noexcept { return _header->mtime; }
    std::uint32_t coverArtId() const noexcept { return _header->coverArtId; }
    std::uint16_t trackNumber() const noexcept { return _header->trackNumber; }
    std::uint16_t totalTracks() const noexcept { return _header->totalTracks; }
    std::uint16_t discNumber() const noexcept { return _header->discNumber; }
    std::uint16_t totalDiscs() const noexcept { return _header->totalDiscs; }
    std::string_view uri() const;

    /**
     * Get all custom key-value pairs.
     * Returns empty vector if data is invalid or empty.
     */
    std::vector<std::pair<std::string, std::string>> customMeta() const;

    /**
     * Look up a custom value by key (exact match, case-sensitive).
     * Returns std::nullopt if key not found.
     */
    std::optional<std::string> customValue(std::string_view key) const;

  private:
    TrackColdHeader const* header() const noexcept { return _header; }
    std::string_view getString(std::uint16_t offset, std::uint16_t len) const;

    TrackColdHeader const* _header = nullptr;
    std::size_t _size = 0;
  };

} // namespace rs::core