// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Exception.h>

#include <ao/Type.h>
#include <ao/library/TrackLayout.h>

#include <ao/utility/ByteView.h>
#include <cstdint>
#include <functional>
#include <gsl-lite/gsl-lite.hpp>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::library
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
  public:
    /**
     * MetadataProxy - Accessors for track metadata ($ prefix).
     * Includes: title, artist, album, genre, year, rating, track info, disc info, uri, cover art.
     */
    class MetadataProxy final
    {
    public:
      explicit MetadataProxy(TrackView const& track)
        : _track{track}
      {
      }

      // From hot
      std::string_view title() const { return _track.hotTitle(); }
      DictionaryId artistId() const noexcept { return _track.hotHeader().artistId; }
      DictionaryId albumId() const noexcept { return _track.hotHeader().albumId; }
      DictionaryId genreId() const noexcept { return _track.hotHeader().genreId; }
      DictionaryId albumArtistId() const noexcept { return _track.hotHeader().albumArtistId; }
      DictionaryId composerId() const noexcept { return _track.hotHeader().composerId; }
      std::uint16_t year() const noexcept { return _track.hotHeader().year; }
      std::uint8_t rating() const noexcept { return _track.hotHeader().rating; }

      // From cold
      std::uint16_t trackNumber() const noexcept { return _track.coldHeader().trackNumber; }
      std::uint16_t totalTracks() const noexcept { return _track.coldHeader().totalTracks; }
      std::uint16_t discNumber() const noexcept { return _track.coldHeader().discNumber; }
      std::uint16_t totalDiscs() const noexcept { return _track.coldHeader().totalDiscs; }
      std::uint32_t coverArtId() const noexcept { return _track.coldHeader().coverArtId; }
      DictionaryId workId() const noexcept { return _track.coldHeader().workId; }

    private:
      TrackView const& _track;
    };

    /**
     * PropertyProxy - Accessors for track properties (@ prefix).
     * Technical audio characteristics: codec, bitDepth, duration, bitrate,
     * sampleRate, channels, fileSize, mtime, uri.
     */
    class PropertyProxy final
    {
    public:
      explicit PropertyProxy(TrackView const& track)
        : _track{track}
      {
      }

      // Hot properties
      std::uint16_t codecId() const noexcept { return _track.hotHeader().codecId; }
      std::uint8_t bitDepth() const noexcept { return _track.hotHeader().bitDepth; }

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
    class TagProxy final : public std::ranges::view_interface<TagProxy>
    {
    public:
      explicit TagProxy(std::span<std::byte const> hotData)
        : _hotData{hotData}
      {
      }

      std::uint8_t count() const noexcept { return hotHeader().tagLen / sizeof(DictionaryId); }
      std::uint32_t bloom() const noexcept { return hotHeader().tagBloom; }

      DictionaryId id(std::uint8_t index) const noexcept
      {
        gsl_Expects(index < count());
        return begin()[index];
      }

      DictionaryId const* begin() const noexcept
      {
        gsl_Expects(_hotData.size() >= sizeof(TrackHotHeader));
        return ao::utility::layout::viewArray<DictionaryId>(
                 _hotData.subspan(sizeof(TrackHotHeader), hotHeader().tagLen))
          .data();
      }

      DictionaryId const* end() const noexcept { return begin() + count(); }
      bool has(DictionaryId tagIdToCheck) const noexcept;

    private:
      TrackHotHeader const& hotHeader() const
      {
        gsl_Expects(_hotData.size() >= sizeof(TrackHotHeader));
        return *ao::utility::layout::view<TrackHotHeader>(_hotData);
      }

      std::span<std::byte const> _hotData;
    };

    /**
     * CustomProxy - Accessors for custom key-value metadata.
     */
    class CustomProxy final : public std::ranges::view_interface<CustomProxy>
    {
    public:
      explicit CustomProxy(std::span<std::byte const> coldData)
        : _coldData{coldData}
      {
      }

      /**
       * Entry - Fixed-size entry in the custom metadata index.
       * 8 bytes total, 4-byte aligned.
       */
      struct Entry
      {
        DictionaryId dictId;  // 4 bytes
        std::uint16_t offset; // 2 bytes - byte offset from header start to value
        std::uint16_t len;    // 2 bytes - value length in bytes
      };

      class Iterator;
      Iterator begin() const;
      Iterator end() const;
      std::optional<std::string_view> get(DictionaryId dictId) const;

    private:
      TrackColdHeader const& coldHeader() const
      {
        gsl_Expects(_coldData.size() >= sizeof(TrackColdHeader));
        return *ao::utility::layout::view<TrackColdHeader>(_coldData);
      }

      std::span<Entry const> entries() const
      {
        constexpr std::size_t kHeaderSize = sizeof(TrackColdHeader);
        gsl_Expects(_coldData.size() >= kHeaderSize);
        auto const entryBytes = static_cast<std::size_t>(coldHeader().customCount) * sizeof(Entry);
        gsl_Expects(kHeaderSize + entryBytes <= _coldData.size());
        return ao::utility::layout::viewArray<Entry>(_coldData.subspan(kHeaderSize, entryBytes));
      }

      std::span<std::byte const> _coldData;
    };

    /**
     * Construct a TrackView with raw bytes.
     *
     * @param id Track ID (LMDB key)
     * @param hotData Hot track binary data (must be >= sizeof(TrackHotHeader)), can be empty span if not loaded
     * @param coldData Cold track binary data (optional), if null accessing cold accessors crashes
     */
    TrackView(std::span<std::byte const> hotData, std::span<std::byte const> coldData)
      : _hotData{hotData}, _coldData{coldData}
    {
    }

    // TrackView is copyable and movable
    TrackView(TrackView&&) = default;
    TrackView& operator=(TrackView&&) = default;
    TrackView(TrackView const&) = default;
    TrackView& operator=(TrackView const&) = default;
    ~TrackView() = default;

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
    TagProxy tags() const { return TagProxy{_hotData}; }
    CustomProxy custom() const { return CustomProxy{_coldData}; }

    // Direct header access
    TrackHotHeader const& hotHeader() const
    {
      gsl_Expects(isHotValid());
      return *ao::utility::layout::view<TrackHotHeader>(_hotData);
    }

    TrackColdHeader const& coldHeader() const
    {
      gsl_Expects(isColdValid());
      return *ao::utility::layout::view<TrackColdHeader>(_coldData);
    }

  private:
    std::string_view hotTitle() const;
    DictionaryId hotTagId(std::uint8_t index) const;
    std::string_view hotGetString(std::uint16_t offset, std::uint16_t len) const;
    std::string_view coldUri() const;
    std::uint64_t coldFileSize() const noexcept;
    std::uint64_t coldMtime() const noexcept;
    std::string_view coldGetString(std::uint16_t offset, std::uint16_t len) const;

    std::span<std::byte const> _hotData;
    std::span<std::byte const> _coldData;
  };

  class TrackView::CustomProxy::Iterator final
  {
  public:
    // Standard iterator traits
    using difference_type = std::ptrdiff_t;
    using value_type = std::pair<DictionaryId const, std::string_view>;
    using reference = value_type;
    using pointer = void;
    using iterator_category = std::forward_iterator_tag;

    Iterator() = default;

    Iterator(CustomProxy::Entry const* pos, std::byte const* coldDataBase);

    value_type operator*() const;

    struct ArrowProxy
    {
      value_type v;
      value_type const* operator->() const { return &v; }
    };

    ArrowProxy operator->() const { return ArrowProxy{**this}; }

    Iterator& operator++();
    Iterator operator++(int);

    bool operator==(Iterator const& other) const { return _pos == other._pos; }

  private:
    CustomProxy::Entry const* _pos = nullptr;
    std::byte const* _coldDataBase = nullptr;
  };
} // namespace ao::library
