// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/Exception.h>

#include <boost/iterator/iterator_facade.hpp>
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
   * TrackView - Unified view of a track with hot and optional cold data.
   *
   * Stores raw binary data and parses on access via proxy classes.
   * If cold data was not provided, accessing cold accessors is undefined behavior (crash).
   * Caller must ensure correct data is provided based on usage.
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
      DictionaryId artistId() const noexcept { return _track.hotHeader().artistId; }
      DictionaryId albumId() const noexcept { return _track.hotHeader().albumId; }
      DictionaryId genreId() const noexcept { return _track.hotHeader().genreId; }
      DictionaryId albumArtistId() const noexcept { return _track.hotHeader().albumArtistId; }
      std::uint16_t year() const noexcept { return _track.hotHeader().year; }

      // From cold
      std::uint16_t trackNumber() const noexcept { return _track.coldHeader().trackNumber; }
      std::uint16_t totalTracks() const noexcept { return _track.coldHeader().totalTracks; }
      std::uint16_t discNumber() const noexcept { return _track.coldHeader().discNumber; }
      std::uint16_t totalDiscs() const noexcept { return _track.coldHeader().totalDiscs; }
      std::uint32_t coverArtId() const noexcept { return _track.coldHeader().coverArtId; }

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
      std::uint16_t codecId() const noexcept { return _track.hotHeader().codecId; }
      std::uint8_t bitDepth() const noexcept { return _track.hotHeader().bitDepth; }
      std::uint8_t rating() const noexcept { return _track.hotHeader().rating; }

      // Cold properties
      std::uint64_t fileSize() const noexcept { return _track.coldFileSize(); }
      std::uint64_t mtime() const noexcept { return _track.coldMtime(); }
      std::uint32_t durationMs() const noexcept { return _track.coldHeader().durationMs; }
      std::uint32_t sampleRate() const noexcept { return _track.coldHeader().sampleRate; }
      std::uint32_t bitrate() const noexcept { return _track.coldHeader().bitrate; }
      std::uint8_t channels() const noexcept { return _track.coldHeader().channels; }
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

      std::uint8_t count() const noexcept { return _track.hotHeader().tagLen / sizeof(DictionaryId); }
      std::uint32_t bloom() const noexcept { return _track.hotHeader().tagBloom; }
      DictionaryId id(std::uint8_t index) const noexcept { return DictionaryId{_track.hotTagId(index)}; }
      DictionaryId const* begin() const noexcept
      {
        return utility::as<DictionaryId>(_track._hotData.subspan(sizeof(TrackHotHeader)));
      }
      DictionaryId const* end() const noexcept { return begin() + count(); }
      bool has(DictionaryId tagIdToCheck) const noexcept;

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

      /**
       * Iterator - Input iterator over custom key-value pairs.
       * Yields std::pair<std::string_view, std::string_view> referencing the cold data buffer.
       */
      class Iterator;
      Iterator begin() const;
      Iterator end() const;
      std::optional<std::string_view> get(std::string_view key) const;

    private:
      std::optional<std::pair<std::byte const*, std::byte const*>> customRange() const;
      TrackView const& _track;
    };

    /**
     * Construct a TrackView with raw bytes.
     *
     * @param id Track ID (LMDB key)
     * @param hotData Hot track binary data (must be >= sizeof(TrackHotHeader)), can be empty span if not loaded
     * @param coldData Cold track binary data (optional), if null accessing cold accessors crashes
     */
    TrackView(std::span<std::byte const> hotData, std::span<std::byte const> coldData)
      : _hotData(hotData)
      , _coldData(coldData)
    {
    }

    // TrackView is movable
    TrackView(TrackView&&) = default;
    TrackView& operator=(TrackView&&) = default;
    TrackView(TrackView const&) = delete;
    TrackView& operator=(TrackView const&) = delete;

    // Hot validity check
    bool isHotValid() const noexcept { return _hotData.data() != nullptr && _hotData.size() >= sizeof(TrackHotHeader); }

    // Cold validity check
    bool isColdValid() const noexcept
    {
      return _coldData.data() != nullptr && _coldData.size() >= sizeof(TrackColdHeader);
    }

    // Accessors
    MetadataProxy metadata() const { return MetadataProxy{*this}; }
    PropertyProxy property() const { return PropertyProxy{*this}; }
    TagProxy tags() const { return TagProxy{*this}; }
    CustomProxy custom() const { return CustomProxy{*this}; }

    // Direct header access
    TrackHotHeader const& hotHeader() const { return *utility::as<TrackHotHeader>(_hotData); }
    TrackColdHeader const& coldHeader() const { return *utility::as<TrackColdHeader>(_coldData); }

  private:
    std::string_view hotTitle() const;
    std::uint32_t hotTagId(std::uint8_t index) const;
    std::string_view hotGetString(std::uint16_t offset, std::uint16_t len) const;
    std::string_view coldUri() const;
    std::uint64_t coldFileSize() const noexcept;
    std::uint64_t coldMtime() const noexcept;
    std::string_view coldGetString(std::uint16_t offset, std::uint16_t len) const;

    std::span<std::byte const> _hotData;
    std::span<std::byte const> _coldData;
  };

  class TrackView::CustomProxy::Iterator
    : public boost::iterator_facade<TrackView::CustomProxy::Iterator,
                                    std::pair<std::string_view, std::string_view> const,
                                    boost::forward_traversal_tag>
  {
  public:
    Iterator() : _currentPos(nullptr), _nextPos(nullptr), _end(nullptr) {}
    Iterator(std::byte const* data, std::byte const* end);

  private:
    friend class boost::iterator_core_access;

    std::pair<std::string_view, std::string_view> const& dereference() const;
    void increment();
    bool equal(Iterator const& other) const;
    static bool decodeEntry(std::byte const* ptr,
                            std::byte const* end,
                            std::pair<std::string_view, std::string_view>& out,
                            std::byte const*& next);

    std::byte const* _currentPos;
    std::byte const* _nextPos;
    std::byte const* _end;
    std::pair<std::string_view, std::string_view> _current;
  };

} // namespace rs::core
