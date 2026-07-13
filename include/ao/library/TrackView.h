// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/PictureType.h>
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
  class TrackView;

  namespace detail
  {
    struct TrackViewRawAccess;
    class TrackColdReader;

    inline constexpr TrackHotHeader kZeroTrackHotHeader{};
    inline constexpr TrackColdHeader kZeroTrackColdHeader{};
  } // namespace detail

  /**
   * ClassicalProxy - Accessors for the classical extension block.
   *
   * Only TrackView and detail::TrackColdReader construct non-empty proxies,
   * from gated slices (at least sizeof(TrackClassicalBlock) bytes, 4-byte
   * aligned); the payload constructor is private to keep that precondition
   * out of reach of ungated data.
   */
  class ClassicalProxy final
  {
  public:
    ClassicalProxy() = default;

    DictionaryId workId() const noexcept { return _block == nullptr ? kInvalidDictionaryId : _block->workId; }
    DictionaryId movementId() const noexcept { return _block == nullptr ? kInvalidDictionaryId : _block->movementId; }
    DictionaryId conductorId() const noexcept { return _block == nullptr ? kInvalidDictionaryId : _block->conductorId; }
    DictionaryId ensembleId() const noexcept { return _block == nullptr ? kInvalidDictionaryId : _block->ensembleId; }
    DictionaryId soloistId() const noexcept { return _block == nullptr ? kInvalidDictionaryId : _block->soloistId; }
    std::uint16_t movementNumber() const noexcept { return _block == nullptr ? 0 : _block->movementNumber; }
    std::uint16_t movementTotal() const noexcept { return _block == nullptr ? 0 : _block->movementTotal; }
    bool empty() const noexcept { return _block == nullptr; }

  private:
    friend class TrackView;
    friend class detail::TrackColdReader;

    explicit ClassicalProxy(std::span<std::byte const> payload) noexcept
      : _block{payload.empty() ? nullptr : utility::layout::view<TrackClassicalBlock>(payload)}
    {
    }

    TrackClassicalBlock const* _block = nullptr;
  };

  /**
   * CoverArtProxy - Ordered, typed cover art entries backed by ResourceStore IDs.
   * The primary cover is the first front cover, or entry 0 when no front cover exists.
   */
  class CoverArtProxy final : public std::ranges::view_interface<CoverArtProxy>
  {
  public:
    CoverArtProxy() = default;

    explicit CoverArtProxy(std::span<CoverArtEntry const> entries) noexcept
      : _entries{entries}
    {
    }

    std::uint16_t count() const noexcept { return static_cast<std::uint16_t>(_entries.size()); }

    CoverArt at(std::uint16_t index) const noexcept
    {
      gsl_Expects(index < count());
      auto const& entry = _entries[index];
      return {.resourceId = entry.id, .type = static_cast<PictureType>(entry.type)};
    }

    /** Returns the front-cover entry, or the first entry, or nullopt if empty. */
    std::optional<CoverArt> primary() const noexcept;

    class Iterator;
    Iterator begin() const;
    Iterator end() const;

  private:
    std::span<CoverArtEntry const> _entries{};
  };

  /**
   * CustomMetadataProxy - Accessors for custom key-value metadata.
   *
   * Only TrackView and detail::TrackColdReader construct non-empty proxies,
   * from gated slices (4-byte aligned, with a CustomMetadataBlockHeader whose
   * entry table fits the payload); the payload constructor is private to keep
   * that precondition out of reach of ungated data. Entry value ranges are
   * clamped per access, so garbage entries yield empty values instead of
   * out-of-bounds reads. Lookup assumes the writer-sorted key order; unsorted
   * (corrupt) entries may miss without being detected.
   */
  class CustomMetadataProxy final : public std::ranges::view_interface<CustomMetadataProxy>
  {
  public:
    CustomMetadataProxy() = default;

    using Entry = CustomMetadataEntry;

    std::uint16_t count() const noexcept
    {
      return _payload.empty() ? 0 : utility::layout::view<CustomMetadataBlockHeader>(_payload)->entryCount;
    }

    class Iterator;
    Iterator begin() const;
    Iterator end() const;
    std::optional<std::string_view> get(DictionaryId dictionaryId) const noexcept;
    bool contains(DictionaryId dictionaryId) const noexcept;

  private:
    friend class TrackView;
    friend class detail::TrackColdReader;

    explicit CustomMetadataProxy(std::span<std::byte const> payload) noexcept
      : _payload{payload}
    {
    }

    std::span<Entry const> entries() const noexcept
    {
      if (_payload.empty())
      {
        return {};
      }

      auto const entryBytes = static_cast<std::size_t>(count()) * sizeof(Entry);
      return utility::layout::viewArray<Entry>(_payload.subspan(sizeof(CustomMetadataBlockHeader), entryBytes));
    }

    static std::string_view value(std::span<std::byte const> payload, Entry const& entry) noexcept;

    std::span<std::byte const> _payload{};
  };

  /**
   * TrackView - Unified view of a track with hot and optional cold data.
   *
   * Record contract (see doc/reference/library/storage/database.md):
   * records are produced exclusively by TrackBuilder and validated at write
   * time; LMDB returns committed bytes unchanged, so the read path performs
   * only an O(1) structural gate per record side. The gate runs once per view
   * (hot eagerly, cold lazily) and establishes that every derived slice stays
   * inside the record span. Accessors never throw and never read out of
   * bounds: when a side is absent or fails its gate, that whole side reads as
   * zero/empty values and isHotValid()/isColdValid() report false. Semantic
   * corruption within the established bounds (for example unsorted custom
   * metadata keys) is not detected here; deep structural verification lives
   * in detail::TrackColdReader for diagnostics and tests.
   *
   * The cold index is a lazy per-view mutable cache; views are intended for
   * single-threaded row access.
   */
  class TrackView final
  {
  public:
    /**
     * MetadataProxy - Accessors for universal track metadata ($ prefix).
     * Classical, cover-art, and custom metadata are exposed through domain accessors on TrackView.
     */
    class MetadataProxy final
    {
    public:
      explicit MetadataProxy(TrackView const& track) noexcept
        : _track{track}
      {
      }

      // From hot
      std::string_view title() const noexcept { return _track.hotTitle(); }
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

    private:
      TrackView const& _track;
    };

    /**
     * PropertyProxy - Accessors for track properties (@ prefix).
     * Technical audio characteristics: codec, bitDepth, duration, bitrate,
     * sampleRate, channels, uri.
     */
    class PropertyProxy final
    {
    public:
      explicit PropertyProxy(TrackView const& track) noexcept
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
      std::string_view uri() const noexcept { return utility::bytes::stringView(_track.coldIndex().uri); }

    private:
      TrackView const& _track;
    };

    /**
     * TagProxy - Accessors for tag data (bloom filter, tag IDs).
     */
    class TagProxy final : public std::ranges::view_interface<TagProxy>
    {
    public:
      TagProxy() = default;

      explicit TagProxy(std::uint32_t bloom, std::span<DictionaryId const> tagIds) noexcept
        : _bloom{bloom}, _tagIds{tagIds}
      {
      }

      // The tag region is bounded by the uint16_t tagLength header field, so
      // a record can hold at most 16383 tags; uint16_t covers the full range.
      std::uint16_t count() const noexcept { return static_cast<std::uint16_t>(_tagIds.size()); }
      std::uint32_t bloom() const noexcept { return _bloom; }

      DictionaryId id(std::uint16_t index) const noexcept
      {
        gsl_Expects(index < count());
        return _tagIds[index];
      }

      DictionaryId const* begin() const noexcept { return _tagIds.data(); }
      DictionaryId const* end() const noexcept { return _tagIds.data() + _tagIds.size(); }
      bool has(DictionaryId tagIdToCheck) const noexcept;

    private:
      std::uint32_t _bloom = 0;
      std::span<DictionaryId const> _tagIds{};
    };

    /**
     * Construct a TrackView over raw record bytes.
     *
     * @param hotData Hot track record, or an empty span when not loaded
     * @param coldData Cold track record, or an empty span when not loaded
     */
    TrackView(std::span<std::byte const> hotData, std::span<std::byte const> coldData) noexcept
      : _hotData{hotData}, _coldData{coldData}, _hotHeader{gateHot(hotData)}
    {
    }

    // TrackView is copyable and movable
    TrackView(TrackView&&) = default;
    TrackView& operator=(TrackView&&) = default;
    TrackView(TrackView const&) = default;
    TrackView& operator=(TrackView const&) = default;
    ~TrackView() = default;

    /** True when the hot record passed its structural gate. */
    bool isHotValid() const noexcept { return _hotHeader != nullptr; }

    /** True when the cold record passed its structural gate. */
    bool isColdValid() const noexcept { return coldIndex().header != nullptr; }

    // Accessors
    MetadataProxy metadata() const noexcept { return MetadataProxy{*this}; }
    PropertyProxy property() const noexcept { return PropertyProxy{*this}; }

    TagProxy tags() const noexcept
    {
      if (_hotHeader == nullptr)
      {
        return TagProxy{};
      }

      return TagProxy{
        _hotHeader->tagBloom,
        utility::layout::viewArray<DictionaryId>(_hotData.subspan(sizeof(TrackHotHeader), _hotHeader->tagLength))};
    }

    ClassicalProxy classical() const noexcept { return ClassicalProxy{coldIndex().classical}; }

    CoverArtProxy coverArt() const noexcept
    {
      return CoverArtProxy{utility::layout::viewArray<CoverArtEntry>(coldIndex().cover)};
    }

    CustomMetadataProxy customMetadata() const noexcept { return CustomMetadataProxy{coldIndex().custom}; }

  private:
    /**
     * ColdIndex - One-shot structural gate result for the cold record.
     * header is non-null iff the gate passed; all slices are then in-bounds,
     * 4-byte aligned, and sized for their proxy preconditions.
     */
    struct ColdIndex final
    {
      TrackColdHeader const* header = nullptr;
      std::span<std::byte const> uri{};
      std::span<std::byte const> cover{};
      std::span<std::byte const> classical{};
      std::span<std::byte const> custom{};
      bool scanned = false;
    };

    /**
     * O(1) hot gate: header fits and is aligned, tag and title extents stay
     * inside the record, and the tag region is a whole number of IDs.
     */
    static TrackHotHeader const* gateHot(std::span<std::byte const> hotData) noexcept
    {
      auto const* header = utility::bytes::tryLayout<TrackHotHeader>(hotData);

      if (header == nullptr || sizeof(TrackHotHeader) + header->tagLength + header->titleLength > hotData.size() ||
          header->tagLength % sizeof(DictionaryId) != 0)
      {
        return nullptr;
      }

      return header;
    }

    TrackHotHeader const& hotHeader() const noexcept
    {
      return _hotHeader != nullptr ? *_hotHeader : detail::kZeroTrackHotHeader;
    }

    TrackColdHeader const& coldHeader() const noexcept
    {
      auto const* header = coldIndex().header;
      return header != nullptr ? *header : detail::kZeroTrackColdHeader;
    }

    std::string_view hotTitle() const noexcept
    {
      if (_hotHeader == nullptr)
      {
        return {};
      }

      return utility::bytes::stringView(
        _hotData.subspan(sizeof(TrackHotHeader) + _hotHeader->tagLength, _hotHeader->titleLength));
    }

    ColdIndex const& coldIndex() const noexcept { return _coldIndex.scanned ? _coldIndex : scanColdIndex(); }
    ColdIndex const& scanColdIndex() const noexcept;

    std::span<std::byte const> hotData() const noexcept { return _hotData; }
    std::span<std::byte const> coldData() const noexcept { return _coldData; }

    std::span<std::byte const> _hotData;
    std::span<std::byte const> _coldData;
    TrackHotHeader const* _hotHeader = nullptr;
    mutable ColdIndex _coldIndex{};

    friend struct detail::TrackViewRawAccess;
  };

  class CustomMetadataProxy::Iterator final
  {
  public:
    // Standard iterator traits
    using difference_type = std::ptrdiff_t;
    using value_type = std::pair<DictionaryId const, std::string_view>;
    using reference = value_type;
    using pointer = void;
    using iterator_category = std::forward_iterator_tag;

    Iterator() = default;

    Iterator(CustomMetadataProxy::Entry const* entry, std::span<std::byte const> payload) noexcept
      : _entry{entry}, _payload{payload}
    {
    }

    value_type operator*() const noexcept { return {_entry->keyId, CustomMetadataProxy::value(_payload, *_entry)}; }

    struct ArrowProxy
    {
      value_type v;
      value_type const* operator->() const { return &v; }
    };

    ArrowProxy operator->() const { return ArrowProxy{**this}; }

    Iterator& operator++() noexcept
    {
      ++_entry;
      return *this;
    }

    Iterator operator++(std::int32_t) noexcept
    {
      auto result = *this;
      ++_entry;
      return result;
    }

    bool operator==(Iterator const& other) const noexcept { return _entry == other._entry; }

  private:
    CustomMetadataProxy::Entry const* _entry = nullptr;
    std::span<std::byte const> _payload{};
  };

  class CoverArtProxy::Iterator final
  {
  public:
    using difference_type = std::ptrdiff_t;
    using value_type = CoverArt;
    using reference = value_type;
    using pointer = void;
    using iterator_category = std::forward_iterator_tag;

    Iterator() = default;
    explicit Iterator(CoverArtEntry const* entry)
      : _entry{entry}
    {
    }

    value_type operator*() const { return {.resourceId = _entry->id, .type = static_cast<PictureType>(_entry->type)}; }

    Iterator& operator++()
    {
      ++_entry;
      return *this;
    }

    Iterator operator++(std::int32_t)
    {
      auto result = *this;
      ++_entry;
      return result;
    }

    bool operator==(Iterator const& other) const { return _entry == other._entry; }

  private:
    CoverArtEntry const* _entry = nullptr;
  };
} // namespace ao::library
