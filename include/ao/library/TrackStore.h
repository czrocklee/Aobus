// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/TrackView.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/ByteView.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace ao::library
{
  namespace detail
  {
    inline bool isFourByteAligned(std::span<std::byte const> bytes) noexcept
    {
      return utility::bytes::isAligned(bytes.data(), 4U);
    }

    // Write-side gate only. The size%4 invariant is load-bearing: together
    // with the 4-byte integer keys and LMDB's node layout it keeps every
    // value in the track databases 4-byte aligned, so read paths can map
    // records with typed views. Read paths do not re-check it; a record that
    // fails TrackView's structural gate reads as an invalid view instead.
    inline Result<> validateSerializedTrackSize(std::size_t size, std::string_view label)
    {
      if ((size % 4U) != 0)
      {
        return makeError(Error::Code::CorruptData, std::string{label} + " track record size is not 4-byte aligned");
      }

      return {};
    }

    inline Result<> validateSerializedTrackBytes(std::span<std::byte const> bytes, std::string_view label)
    {
      if (auto sizeResult = validateSerializedTrackSize(bytes.size(), label); !sizeResult)
      {
        return sizeResult;
      }

      if (!isFourByteAligned(bytes))
      {
        return makeError(Error::Code::CorruptData, std::string{label} + " track record pointer is not 4-byte aligned");
      }

      return {};
    }
  } // namespace detail

  /**
   * TrackStore - Binary storage for tracks using hot/cold separation.
   *
   * Uses two LMDB databases:
   * - tracks_hot: TrackHotHeader + payload (hot fields for fast filtering)
   * - tracks_cold: TrackColdHeader + optional cold payloads + uri (cold fields)
   * - Key: uint32_t track ID (same ID links hot and cold)
   */
  class TrackStore final
  {
  public:
    class Reader;
    class Writer;

    explicit TrackStore(lmdb::Database hotDb, lmdb::Database coldDb);

    Reader reader(lmdb::ReadTransaction const& transaction) const;
    Writer writer(lmdb::WriteTransaction& transaction);

  private:
    lmdb::Database _hotDb;
    lmdb::Database _coldDb;
  };

  /**
   * TrackStore::Reader - Read-only access to tracks.
   */
  class TrackStore::Reader final
  {
  public:
    class Iterator;

    /**
     * LoadMode - Controls which data is loaded for each track.
     */
    enum class LoadMode : std::uint8_t
    {
      Hot,  // Only hot data
      Cold, // Only cold data
      Both  // Both hot and cold
    };

    struct EndSentinel
    {};

    explicit Reader(lmdb::Database::Reader hotReader, lmdb::Database::Reader coldReader);

    Iterator begin(LoadMode mode = LoadMode::Both) const;
    Iterator end(LoadMode mode = LoadMode::Both) const;

    /**
     * Get a track by ID.
     * @return TrackView, or std::nullopt if the track is missing. Storage
     *         faults throw (see lmdb). A structurally corrupt record is
     *         still returned; it reads as an invalid view side
     *         (isHotValid()/isColdValid() false, accessors yield defaults).
     */
    std::optional<TrackView> get(TrackId id, LoadMode mode = LoadMode::Both) const;

    auto hot() const;
    auto cold() const;
    auto both() const;

    lmdb::Database::Reader const& hotReader() const noexcept { return _hotReader; }
    lmdb::Database::Reader const& coldReader() const noexcept { return _coldReader; }

  private:
    lmdb::Database::Reader _hotReader;
    lmdb::Database::Reader _coldReader;
    friend class TrackStore;
  };

  /**
   * TrackStore::Reader::Iterator - Iterator over tracks.
   */
  class TrackStore::Reader::Iterator final
  {
  public:
    using value_type = std::pair<TrackId, TrackView>;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::input_iterator_tag;

    Iterator() = default;
    Iterator(Iterator const&) = delete;
    ~Iterator() = default;
    Iterator(Iterator&&) = default;

    Iterator& operator=(Iterator const&) = delete;
    Iterator& operator=(Iterator&&) = default;

    bool operator==(Iterator const& other) const;
    bool operator==(EndSentinel /*unused*/) const;
    Iterator& operator++();
    void operator++(std::int32_t) { ++*this; }
    value_type operator*() const;

  private:
    Iterator(lmdb::Database::Reader::Iterator&& hotIter,
             lmdb::Database::Reader::Iterator&& coldIter,
             Reader::LoadMode mode);

    std::optional<lmdb::Database::Reader::Iterator> _optHotIter;
    std::optional<lmdb::Database::Reader::Iterator> _optColdIter;
    Reader::LoadMode _mode = Reader::LoadMode::Both;
    friend class Reader;
  };

  inline auto TrackStore::Reader::hot() const
  {
    return std::ranges::subrange{begin(LoadMode::Hot), EndSentinel{}};
  }

  inline auto TrackStore::Reader::cold() const
  {
    return std::ranges::subrange{begin(LoadMode::Cold), EndSentinel{}};
  }

  inline auto TrackStore::Reader::both() const
  {
    return std::ranges::subrange{begin(LoadMode::Both), EndSentinel{}};
  }

  /**
   * TrackStore::Writer - Write access to tracks.
   */
  class TrackStore::Writer final
  {
  public:
    /**
     * Get track by ID with specified load mode.
     * @return TrackView, or std::nullopt if the track is missing.
     */
    std::optional<TrackView> get(TrackId id, Reader::LoadMode mode) const;

    /**
     * Create a new track with hot and cold data.
     * @param hotData TrackHotHeader + payload
     * @param coldData TrackColdHeader + optional cold payloads + uri
     * @return Pair of (track ID, TrackView)
     */
    Result<std::pair<TrackId, TrackView>> createHotCold(std::span<std::byte const> hotData,
                                                        std::span<std::byte const> coldData);

    /**
     * Zero-copy create: reserves space and calls fill callback to populate spans.
     * @param hotSize Size of hot data (must be multiple of 4)
     * @param coldSize Size of cold data (must be multiple of 4)
     * @param fill Callback: fill(TrackId id, span<byte> hot, span<byte> cold) -> void
     * @return Pair of (track ID, TrackView)
     */
    template<typename F>
      requires std::invocable<F, TrackId, std::span<std::byte>, std::span<std::byte>>
    Result<std::pair<TrackId, TrackView>> createHotCold(std::size_t hotSize, std::size_t coldSize, F&& fill);

    /**
     * Update hot track data.
     */
    Result<> updateHot(TrackId id, std::span<std::byte const> hotData);

    /**
     * Zero-copy updateHot: reserves space and calls fill callback to populate span.
     * @param id Track ID to update
     * @param size Size of hot data (must be multiple of 4)
     * @param fill Callback: fill(span<byte> hot) -> void
     */
    template<typename F>
      requires std::invocable<F, std::span<std::byte>>
    Result<> updateHot(TrackId id, std::size_t size, F&& fill);

    /**
     * Update cold track data (direct span access).
     */
    Result<std::span<std::byte>> updateCold(TrackId id, std::size_t size);

    /**
     * Zero-copy updateCold: reserves space and calls fill callback to populate span.
     * @param id Track ID to update
     * @param size Size of cold data (must be multiple of 4)
     * @param fill Callback: fill(span<byte> cold) -> void
     */
    template<typename F>
      requires std::invocable<F, std::span<std::byte>>
    Result<> updateCold(TrackId id, std::size_t size, F&& fill);

    /**
     * Delete both hot and cold track data.
     * @return true if a row was removed, false if the id was absent.
     */
    bool remove(TrackId id);

    /**
     * Clear all tracks.
     */
    Result<> clear();

    lmdb::Database::Writer& hotWriter() noexcept { return _hotWriter; }
    lmdb::Database::Writer& coldWriter() noexcept { return _coldWriter; }

  private:
    explicit Writer(lmdb::Database::Writer&& hotWriter, lmdb::Database::Writer&& coldWriter);

    lmdb::Database::Writer _hotWriter;
    lmdb::Database::Writer _coldWriter;
    friend class TrackStore;
  };

  // Template implementations

  template<typename F>
    requires std::invocable<F, TrackId, std::span<std::byte>, std::span<std::byte>>
  Result<std::pair<TrackId, TrackView>> TrackStore::Writer::createHotCold(std::size_t hotSize,
                                                                          std::size_t coldSize,
                                                                          F&& fill)
  {
    if (auto validation = detail::validateSerializedTrackSize(hotSize, "hot"); !validation)
    {
      return std::unexpected{validation.error()};
    }

    if (auto validation = detail::validateSerializedTrackSize(coldSize, "cold"); !validation)
    {
      return std::unexpected{validation.error()};
    }

    // Reserve hot span and get auto-increment ID
    auto hotResult = _hotWriter.append(hotSize);

    if (!hotResult)
    {
      return std::unexpected{hotResult.error()};
    }

    auto [id, hotSpan] = *hotResult;

    if (auto validation = detail::validateSerializedTrackBytes(hotSpan, "hot"); !validation)
    {
      return std::unexpected{validation.error()};
    }

    // Reserve cold at the SAME explicit ID (not append)
    auto coldResult = _coldWriter.create(id, coldSize);

    if (!coldResult)
    {
      return std::unexpected{coldResult.error()};
    }

    auto coldSpan = *coldResult;

    if (auto validation = detail::validateSerializedTrackBytes(coldSpan, "cold"); !validation)
    {
      return std::unexpected{validation.error()};
    }

    // Populate both spans via callback
    std::invoke(std::forward<F>(fill), TrackId{id}, hotSpan, coldSpan);

    return std::pair{TrackId{id}, TrackView{hotSpan, coldSpan}};
  }

  template<typename F>
    requires std::invocable<F, std::span<std::byte>>
  Result<> TrackStore::Writer::updateHot(TrackId id, std::size_t size, F&& fill)
  {
    if (auto validation = detail::validateSerializedTrackSize(size, "hot"); !validation)
    {
      return validation;
    }

    auto spanResult = _hotWriter.update(id.raw(), size);

    if (!spanResult)
    {
      return std::unexpected{spanResult.error()};
    }

    auto span = *spanResult;

    if (auto validation = detail::validateSerializedTrackBytes(span, "hot"); !validation)
    {
      return validation;
    }

    std::invoke(std::forward<F>(fill), span);
    return {};
  }

  template<typename F>
    requires std::invocable<F, std::span<std::byte>>
  Result<> TrackStore::Writer::updateCold(TrackId id, std::size_t size, F&& fill)
  {
    if (auto validation = detail::validateSerializedTrackSize(size, "cold"); !validation)
    {
      return validation;
    }

    auto spanResult = _coldWriter.update(id.raw(), size);

    if (!spanResult)
    {
      return std::unexpected{spanResult.error()};
    }

    auto span = *spanResult;

    if (auto validation = detail::validateSerializedTrackBytes(span, "cold"); !validation)
    {
      return validation;
    }

    std::invoke(std::forward<F>(fill), span);
    return {};
  }
} // namespace ao::library
