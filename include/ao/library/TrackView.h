// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/library/CoverArt.h>
#include <ao/library/TrackLayout.h>
#include <ao/utility/ByteView.h>

#include <gsl-lite/gsl-lite.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>

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
     * Includes: title, artist, album, genre, year, track info, disc info, uri, cover art.
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

      // From cold
      std::uint16_t trackNumber() const noexcept { return _track.coldHeader().trackNumber; }
      std::uint16_t trackTotal() const noexcept { return _track.coldHeader().trackTotal; }
      std::uint16_t discNumber() const noexcept { return _track.coldHeader().discNumber; }
      std::uint16_t discTotal() const noexcept { return _track.coldHeader().discTotal; }
      DictionaryId workId() const noexcept { return _track.coldHeader().workId; }
      DictionaryId movementId() const noexcept { return _track.coldHeader().movementId; }
      std::uint16_t movementNumber() const noexcept { return _track.coldHeader().movementNumber; }
      std::uint16_t movementTotal() const noexcept { return _track.coldHeader().movementTotal; }

    private:
      TrackView const& _track;
    };

    /**
     * CoverArtProxy - Ordered, typed cover art entries backed by ResourceStore IDs.
     * The primary cover is the first front cover, or entry 0 when no front cover exists.
     */
    class CoverArtProxy final : public std::ranges::view_interface<CoverArtProxy>
    {
    public:
      explicit CoverArtProxy(std::span<std::byte const> coldData)
        : _coldData{coldData}
      {
      }

      std::uint16_t count() const noexcept { return coldHeader().coverCount; }

      CoverArt at(std::uint16_t index) const noexcept
      {
        gsl_Expects(index < count());
        auto const& entry = entries()[index];
        return {.resourceId = entry.id, .type = static_cast<PictureType>(entry.type)};
      }

      /** Returns the front-cover entry, or the first entry, or nullopt if empty. */
      std::optional<CoverArt> primary() const noexcept;

      class Iterator;
      Iterator begin() const;
      Iterator end() const;

    private:
      TrackColdHeader const& coldHeader() const
      {
        gsl_Expects(_coldData.size() >= sizeof(TrackColdHeader));
        return *utility::layout::view<TrackColdHeader>(_coldData);
      }

      std::span<CoverArtEntry const> entries() const
      {
        auto const& hdr = coldHeader();
        auto const entryBytes = static_cast<std::size_t>(hdr.coverCount) * sizeof(CoverArtEntry);
        constexpr std::size_t kHeaderSize = sizeof(TrackColdHeader);

        gsl_Expects(kHeaderSize + entryBytes <= _coldData.size());
        return utility::layout::viewArray<CoverArtEntry>(_coldData.subspan(kHeaderSize, entryBytes));
      }

      std::span<std::byte const> _coldData;
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
      AudioCodec codec() const noexcept { return _track.hotHeader().codec; }
      BitDepth bitDepth() const noexcept { return _track.hotHeader().bitDepth; }
      SampleRate sampleRate() const noexcept { return _track.hotHeader().sampleRate; }

      // Technical properties (from cold)
      std::chrono::milliseconds duration() const noexcept { return _track.coldHeader().duration; }
      Bitrate bitrate() const noexcept { return _track.coldHeader().bitrate; }
      Channels channels() const noexcept { return _track.coldHeader().channels; }
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

      std::uint8_t count() const noexcept
      {
        auto const& header = hotHeader();
        return static_cast<std::uint8_t>(header.tagLength / sizeof(DictionaryId));
      }

      std::uint32_t bloom() const noexcept { return hotHeader().tagBloom; }

      DictionaryId id(std::uint8_t index) const noexcept
      {
        gsl_Expects(index < count());
        return begin()[index];
      }

      DictionaryId const* begin() const noexcept
      {
        auto const& header = hotHeader();
        return utility::layout::viewArray<DictionaryId>(_hotData.subspan(sizeof(TrackHotHeader), header.tagLength))
          .data();
      }

      DictionaryId const* end() const noexcept { return begin() + count(); }
      bool has(DictionaryId tagIdToCheck) const noexcept;

    private:
      TrackHotHeader const& hotHeader() const;

      std::span<std::byte const> _hotData;
    };

    /**
     * CustomMetadataProxy - Accessors for custom key-value metadata.
     */
    class CustomMetadataProxy final : public std::ranges::view_interface<CustomMetadataProxy>
    {
    public:
      explicit CustomMetadataProxy(std::span<std::byte const> coldData)
        : _coldData{coldData}
      {
      }

      using Entry = CustomMetadataEntry;

      class Iterator;
      Iterator begin() const;
      Iterator end() const;
      std::optional<std::string_view> get(DictionaryId dictId) const;
      bool contains(DictionaryId dictId) const;

    private:
      TrackColdHeader const& coldHeader() const
      {
        gsl_Expects(_coldData.size() >= sizeof(TrackColdHeader));
        return *utility::layout::view<TrackColdHeader>(_coldData);
      }

      std::span<Entry const> entries() const
      {
        auto const& hdr = coldHeader();
        auto const entryBytes = static_cast<std::size_t>(hdr.customCount) * sizeof(Entry);
        auto const customOffset = static_cast<std::size_t>(hdr.customOffset);

        gsl_Expects(customOffset + entryBytes <= _coldData.size());
        return utility::layout::viewArray<Entry>(_coldData.subspan(customOffset, entryBytes));
      }

      std::span<std::byte const> _coldData;
    };

    /**
     * Construct a TrackView with raw bytes.
     *
     * @param hotData Hot track binary data (must be >= sizeof(TrackHotHeader)), can be empty span if not loaded
     * @param coldData Cold track binary data (optional), if null accessing cold accessors crashes
     * @param fileSize File size in bytes (optional, from manifest)
     * @param mtime Modification time (optional, from manifest)
     * @param status Physical availability of the file
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
    CustomMetadataProxy customMetadata() const { return CustomMetadataProxy{_coldData}; }
    CoverArtProxy coverArt() const { return CoverArtProxy{_coldData}; }

    // Direct header access
    std::span<std::byte const> hotData() const noexcept { return _hotData; }
    std::span<std::byte const> coldData() const noexcept { return _coldData; }

    TrackHotHeader const& hotHeader() const;

    TrackColdHeader const& coldHeader() const;

  private:
    std::string_view hotTitle() const;
    DictionaryId hotTagId(std::uint8_t index) const;
    std::string_view hotGetString(std::uint16_t offset, std::uint16_t len) const;
    std::string_view coldUri() const;
    std::string_view coldGetString(std::uint16_t offset, std::uint16_t len) const;

    std::span<std::byte const> _hotData;
    std::span<std::byte const> _coldData;
  };

  class TrackView::CustomMetadataProxy::Iterator final
  {
  public:
    // Standard iterator traits
    using difference_type = std::ptrdiff_t;
    using value_type = std::pair<DictionaryId const, std::string_view>;
    using reference = value_type;
    using pointer = void;
    using iterator_category = std::forward_iterator_tag;

    Iterator() = default;

    Iterator(CustomMetadataProxy::Entry const* pos, std::byte const* coldDataBase);

    value_type operator*() const;

    struct ArrowProxy
    {
      value_type v;
      value_type const* operator->() const { return &v; }
    };

    ArrowProxy operator->() const { return ArrowProxy{**this}; }

    Iterator& operator++();
    Iterator operator++(std::int32_t);

    bool operator==(Iterator const& other) const { return _pos == other._pos; }

  private:
    CustomMetadataProxy::Entry const* _pos = nullptr;
    std::byte const* _coldDataBase = nullptr;
  };

  class TrackView::CoverArtProxy::Iterator final
  {
  public:
    using difference_type = std::ptrdiff_t;
    using value_type = CoverArt;
    using reference = value_type;
    using pointer = void;
    using iterator_category = std::forward_iterator_tag;

    Iterator() = default;
    explicit Iterator(CoverArtEntry const* pos)
      : _pos{pos}
    {
    }

    value_type operator*() const { return {.resourceId = _pos->id, .type = static_cast<PictureType>(_pos->type)}; }

    Iterator& operator++()
    {
      ++_pos;
      return *this;
    }

    Iterator operator++(std::int32_t)
    {
      auto result = *this;
      ++_pos;
      return result;
    }

    bool operator==(Iterator const& other) const { return _pos == other._pos; }

  private:
    CoverArtEntry const* _pos = nullptr;
  };
} // namespace ao::library
