// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/TrackView.h>
#include <ao/lmdb/Database.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <utility>

namespace ao::library
{
  class ReadTransaction;
  class LibraryWrite;
  class WriteTransaction;
  class MusicLibrary;

  namespace detail
  {
    class LibraryIdentity;
    class PhysicalStoreAccess;
    class WriteTransactionAccess;
    class TrackWriteAccess;
  }

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

    Reader reader(ReadTransaction const& transaction) const;
    Reader reader(LibraryWrite const& write) const;
    Reader reader(WriteTransaction const& transaction) const;

  private:
    Writer writer(WriteTransaction& transaction) const;
    TrackStore(lmdb::IntegerKeyDatabase hotDb,
               lmdb::IntegerKeyDatabase coldDb,
               detail::LibraryIdentity const& identity);

    lmdb::IntegerKeyDatabase _hotDb;
    lmdb::IntegerKeyDatabase _coldDb;
    detail::LibraryIdentity const* _identity;

    friend class MusicLibrary;
    friend class detail::WriteTransactionAccess;
    friend class detail::PhysicalStoreAccess;
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

    Iterator begin() const;
    EndSentinel end() const { return {}; }

    /**
     * Get a track by ID.
     * @return TrackView, or std::nullopt only when the requested Track is
     *         absent. A post-open native read fault is fatal, and a loaded
     *         side that violates the open-time structural proof fails an
     *         invariant before this method returns. In Both mode, neither
     *         side present is absence; exactly one side present violates the
     *         validated Track-pair invariant.
     */
    std::optional<TrackView> get(TrackId id, LoadMode mode = LoadMode::Both) const;

    /** Number of complete Track rows visible in this snapshot. */
    std::size_t entryCount() const;

    /**
     * Visit tracks selected by ID, preserving the requested order.
     *
     * Missing rows are skipped. Duplicate IDs therefore produce duplicate
     * visits when the row exists. A loaded side that violates the open-time
     * structural proof fails an invariant before visitor invocation; it is
     * never converted into a miss or delivered as a poisoned Store row.
     * Views remain transaction-scoped, like get(). Strictly ascending dense
     * selections may use cursor traversal internally; sparse or arbitrarily
     * ordered selections retain point-lookup behavior.
     */
    template<typename Visitor>
      requires std::invocable<Visitor&, TrackId, TrackView const&>
    void visitTracks(std::span<TrackId const> ids, LoadMode mode, Visitor visitor) const;

    auto hot() const;
    auto cold() const;

  private:
    explicit Reader(lmdb::IntegerKeyDatabase::Reader hotReader, lmdb::IntegerKeyDatabase::Reader coldReader);

    Iterator beginFor(LoadMode mode) const;
    bool shouldUseCursorScan(std::span<TrackId const> ids, LoadMode mode) const;

    lmdb::IntegerKeyDatabase::Reader _hotReader;
    lmdb::IntegerKeyDatabase::Reader _coldReader;
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
    Iterator(lmdb::IntegerKeyDatabase::Reader::Iterator&& hotIter,
             lmdb::IntegerKeyDatabase::Reader::Iterator&& coldIter,
             Reader::LoadMode mode);

    void validateBothPosition() const;

    lmdb::IntegerKeyDatabase::Reader::Iterator _hotIter;
    lmdb::IntegerKeyDatabase::Reader::Iterator _coldIter;
    Reader::LoadMode _mode = Reader::LoadMode::Both;
    friend class Reader;
  };

  inline auto TrackStore::Reader::hot() const
  {
    return std::ranges::subrange{beginFor(LoadMode::Hot), EndSentinel{}};
  }

  inline auto TrackStore::Reader::cold() const
  {
    return std::ranges::subrange{beginFor(LoadMode::Cold), EndSentinel{}};
  }

  template<typename Visitor>
    requires std::invocable<Visitor&, TrackId, TrackView const&>
  void TrackStore::Reader::visitTracks(std::span<TrackId const> ids, LoadMode mode, Visitor visitor) const
  {
    if (!shouldUseCursorScan(ids, mode))
    {
      for (auto const id : ids)
      {
        if (auto const optView = get(id, mode); optView)
        {
          std::invoke(visitor, id, *optView);
        }
      }

      return;
    }

    auto requested = ids.begin();

    for (auto iterator = beginFor(mode); iterator != EndSentinel{} && requested != ids.end(); ++iterator)
    {
      auto&& [storedId, view] = *iterator;

      while (requested != ids.end() && *requested < storedId)
      {
        ++requested;
      }

      if (requested == ids.end())
      {
        break;
      }

      if (*requested == storedId)
      {
        std::invoke(visitor, storedId, view);
        ++requested;
      }
    }
  }

  /**
   * TrackStore::Writer - Write access to tracks.
   *
   * Physical record creation and replacement live inside ao_library and take
   * TrackBuilder::PreparedHot/PreparedCold: the record is serialized straight
   * into storage-owned bytes and validated there, so no entry point here
   * accepts caller-supplied record bytes.
   */
  class [[nodiscard]] TrackStore::Writer final
  {
  public:
    /**
     * Get track by ID with specified load mode.
     * @return TrackView, or std::nullopt if the selected row is missing. In
     *         Both mode, exactly one side present violates the validated
     *         Track-pair invariant.
     */
    std::optional<TrackView> get(TrackId id, Reader::LoadMode mode = Reader::LoadMode::Both) const;

    /**
     * Delete both hot and cold track data.
     * @return true if a row was removed, false if the id was absent.
     */
    bool remove(TrackId id);

    /**
     * Clear all tracks.
     */
    Result<> clear();

  private:
    Writer(lmdb::IntegerKeyDatabase::Writer hotWriter, lmdb::IntegerKeyDatabase::Writer coldWriter);

    lmdb::IntegerKeyDatabase::Writer _hotWriter;
    lmdb::IntegerKeyDatabase::Writer _coldWriter;

    friend class TrackStore;
    friend class detail::TrackWriteAccess;
  };
} // namespace ao::library
