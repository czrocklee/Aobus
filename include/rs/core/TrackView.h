// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/Exception.h>
#include <rs/core/TrackLayout.h>
#include <rs/core/Type.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <rs/utility/ByteView.h>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace rs::core
{

  /**
   * ColdLoadHint - Controls when cold data is loaded for TrackView.
   */
  enum class ColdLoadHint
  {
    Lazy,  // Don't load cold data, load on demand
    Eager  // Load cold data immediately
  };

  /**
   * TrackView - Unified view of a track with hot and optional cold data.
   *
   * Stores raw binary data and parses on access via proxy classes.
   * Provides lazy loading of cold data - cold is only loaded when accessed
   * if not provided at construction time.
   *
   * Use ColdLoadHint::Lazy when iterating for hot-only queries (performance).
   * Use ColdLoadHint::Eager when you need cold data for every track.
   */
  class TrackView final
  {
    // Friend declarations for proxy classes
    friend class MetadataProxy;
    friend class PropertyProxy;
    friend class TagProxy;
    friend class CustomProxy;

  public:
    /**
     * MetadataProxy - Accessors for track metadata ($ prefix).
     * Includes: title, artist, album, genre, year, track info, disc info, uri, cover art.
     */
    class MetadataProxy
    {
    public:
      explicit MetadataProxy(TrackView const& track) : _track(track) {}

      // From hot
      std::string_view title() const { return _track.hotTitle(); }
      DictionaryId artistId() const noexcept { return _track.hotHeader()->artistId; }
      DictionaryId albumId() const noexcept { return _track.hotHeader()->albumId; }
      DictionaryId genreId() const noexcept { return _track.hotHeader()->genreId; }
      DictionaryId albumArtistId() const noexcept { return _track.hotHeader()->albumArtistId; }
      std::uint16_t year() const noexcept { return _track.hotHeader()->year; }

      // From cold
      std::uint16_t trackNumber() const noexcept { return _track.coldHeader()->trackNumber; }
      std::uint16_t totalTracks() const noexcept { return _track.coldHeader()->totalTracks; }
      std::uint16_t discNumber() const noexcept { return _track.coldHeader()->discNumber; }
      std::uint16_t totalDiscs() const noexcept { return _track.coldHeader()->totalDiscs; }
      std::uint32_t coverArtId() const noexcept { return _track.coldHeader()->coverArtId; }

    private:
      TrackView const& _track;
    };

    /**
     * PropertyProxy - Accessors for track properties (@ prefix).
     * Technical audio characteristics: codec, bitDepth, rating, duration, bitrate,
     * sampleRate, channels, fileSize, mtime, uri.
     */
    class PropertyProxy
    {
    public:
      explicit PropertyProxy(TrackView const& track) : _track(track) {}

      // Hot properties
      std::uint16_t codecId() const noexcept { return _track.hotHeader()->codecId; }
      std::uint8_t bitDepth() const noexcept { return _track.hotHeader()->bitDepth; }
      std::uint8_t rating() const noexcept { return _track.hotHeader()->rating; }

      // Cold properties
      std::uint64_t fileSize() const noexcept { return _track.coldFileSize(); }
      std::uint64_t mtime() const noexcept { return _track.coldMtime(); }
      std::uint32_t durationMs() const noexcept { return _track.coldHeader()->durationMs; }
      std::uint32_t sampleRate() const noexcept { return _track.coldHeader()->sampleRate; }
      std::uint32_t bitrate() const noexcept { return _track.coldHeader()->bitrate; }
      std::uint8_t channels() const noexcept { return _track.coldHeader()->channels; }
      std::string_view uri() const { return _track.coldUri(); }

    private:
      TrackView const& _track;
    };

    /**
     * TagProxy - Accessors for tag data (bloom filter, tag IDs).
     */
    class TagProxy
    {
    public:
      explicit TagProxy(TrackView const& track) : _track(track) {}

      std::uint8_t count() const noexcept { return _track.hotHeader()->tagCount; }
      std::uint32_t bloom() const noexcept { return _track.hotHeader()->tagBloom; }
      DictionaryId id(std::uint8_t index) const { return DictionaryId{_track.hotTagId(index)}; }
      std::vector<DictionaryId> ids() const;
      bool has(DictionaryId tagIdToCheck) const;

    private:
      TrackView const& _track;
    };

    /**
     * ColdProxy - Accessors for cold data (triggers lazy load on construction).
     * Provides unified access to all cold properties.
     */
    class ColdProxy
    {
    public:
      explicit ColdProxy(TrackView const& track) : _track(track) { _track.ensureColdLoaded(); }

      // All cold properties
      std::uint64_t fileSize() const noexcept { return _track.coldFileSize(); }
      std::uint64_t mtime() const noexcept { return _track.coldMtime(); }
      std::uint32_t durationMs() const noexcept { return _track.coldHeader()->durationMs; }
      std::uint32_t sampleRate() const noexcept { return _track.coldHeader()->sampleRate; }
      std::uint32_t coverArtId() const noexcept { return _track.coldHeader()->coverArtId; }
      std::uint32_t bitrate() const noexcept { return _track.coldHeader()->bitrate; }
      std::uint16_t trackNumber() const noexcept { return _track.coldHeader()->trackNumber; }
      std::uint16_t totalTracks() const noexcept { return _track.coldHeader()->totalTracks; }
      std::uint16_t discNumber() const noexcept { return _track.coldHeader()->discNumber; }
      std::uint16_t totalDiscs() const noexcept { return _track.coldHeader()->totalDiscs; }
      std::uint8_t channels() const noexcept { return _track.coldHeader()->channels; }
      std::string_view uri() const { return _track.coldUri(); }

    private:
      TrackView const& _track;
    };

    /**
     * CustomProxy - Accessors for custom key-value metadata.
     */
    class CustomProxy
    {
    public:
      explicit CustomProxy(TrackView const& track) : _track(track) {}

      std::vector<std::pair<std::string, std::string>> all() const;
      std::optional<std::string> get(std::string_view key) const;

    private:
      TrackView const& _track;
    };

    /**
     * Construct a TrackView with raw bytes.
     *
     * @param id Track ID (LMDB key)
     * @param hotData Hot track binary data (must be >= sizeof(TrackHotHeader))
     * @param coldData Cold track binary data (optional, for eager loading)
     * @param coldLoader Function to lazily load cold data (optional)
     */
    TrackView(TrackId id,
              std::span<std::byte const> hotData,
              std::optional<std::span<std::byte const>> coldData = std::nullopt,
              std::function<std::optional<std::span<std::byte const>>(TrackId)> coldLoader = nullptr)
      : _id(id)
      , _hotData(hotData)
      , _coldData(coldData)
      , _coldLoaded(coldData.has_value())
      , _coldLoader(std::move(coldLoader))
    {}

    // TrackView is movable
    TrackView(TrackView&&) = default;
    TrackView& operator=(TrackView&&) = default;
    TrackView(TrackView const&) = delete;
    TrackView& operator=(TrackView const&) = delete;

    // Track ID (LMDB key)
    TrackId id() const { return _id; }

    // Validity checks
    bool isHotValid() const noexcept {
      return _hotData.data() != nullptr && _hotData.size() >= sizeof(TrackHotHeader);
    }

    // Check if cold data is available (loaded or loader exists)
    bool hasCold() const { return _coldData.has_value() || _coldLoader != nullptr; }

    // Check if cold data has been loaded
    bool isColdLoaded() const { return _coldLoaded; }

    // Accessors - hot always available, cold lazy-loaded
    MetadataProxy metadata() const { return MetadataProxy{*this}; }
    PropertyProxy property() const { return PropertyProxy{*this}; }
    TagProxy tags() const { return TagProxy{*this}; }
    ColdProxy cold() const { return ColdProxy{*this}; }
    CustomProxy custom() const { return CustomProxy{*this}; }

    // Direct header access (hot always available)
    TrackHotHeader const* hotHeader() const {
      return utility::as<TrackHotHeader>(_hotData);
    }

    // Cold header access (triggers lazy load)
    TrackColdHeader const* coldHeader() const {
      ensureColdLoaded();
      if (!_coldData || _coldData->size() < sizeof(TrackColdHeader)) {
        return nullptr;
      }
      return utility::as<TrackColdHeader>(*_coldData);
    }

  private:
    void ensureColdLoaded() const {
      if (!_coldLoaded && _coldLoader) {
        _coldData = _coldLoader(_id);
        _coldLoaded = true;
      }
    }

    std::string_view hotTitle() const;
    std::uint32_t hotTagId(std::uint8_t index) const;
    std::string_view hotGetString(std::uint16_t offset, std::uint16_t len) const;
    std::string_view coldUri() const;
    std::uint64_t coldFileSize() const noexcept;
    std::uint64_t coldMtime() const noexcept;
    std::string_view coldGetString(std::uint16_t offset, std::uint16_t len) const;
    std::vector<std::pair<std::string, std::string>> coldCustomMeta() const;
    std::optional<std::string> coldCustomValue(std::string_view key) const;

    TrackId _id;
    std::span<std::byte const> _hotData;
    mutable std::optional<std::span<std::byte const>> _coldData;
    mutable bool _coldLoaded = false;
    std::function<std::optional<std::span<std::byte const>>(TrackId)> _coldLoader;
  };

} // namespace rs::core
