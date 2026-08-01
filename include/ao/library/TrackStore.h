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
  class WriteTransaction;
  class MusicLibrary;

  namespace detail
  {
    class LibraryIdentity;
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
    Reader reader(WriteTransaction const& transaction) const;
    Writer writer(WriteTransaction& transaction) const;

  private:
    TrackStore(lmdb::Database hotDb, lmdb::Database coldDb, detail::LibraryIdentity const& identity);

    lmdb::Database _hotDb;
    lmdb::Database _coldDb;
    detail::LibraryIdentity const* _identity;

    friend class MusicLibrary;
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

    Iterator begin(LoadMode mode = LoadMode::Both) const;
    Iterator end(LoadMode mode = LoadMode::Both) const;

    /**
     * Get a track by ID.
     * @return TrackView, or std::nullopt if the track is missing. Storage
     *         faults throw (see lmdb). A structurally corrupt record is
     *         still returned with the corresponding validity query false.
     *         Callers must check the loaded side before invoking its decoded
     *         accessors; doing otherwise violates the TrackView contract.
     */
    std::optional<TrackView> get(TrackId id, LoadMode mode = LoadMode::Both) const;

    /** Number of rows visible for the selected load mode. */
    std::size_t entryCount(LoadMode mode = LoadMode::Both) const;

    /**
     * Visit tracks selected by ID, preserving the requested order.
     *
     * Missing rows are skipped. Duplicate IDs therefore produce duplicate
     * visits when the row exists. A structurally malformed loaded side is
     * delivered as a TrackView whose corresponding validity query is false;
     * the visitor must check that query before using decoded accessors. Such a
     * visitor-side contract failure is not converted into a missing row, and
     * earlier visitor side effects are not rolled back. Views remain
     * transaction-scoped, like get(). Strictly ascending dense selections may
     * use cursor traversal internally; sparse or arbitrarily ordered
     * selections retain point-lookup behavior.
     */
    template<typename Visitor>
      requires std::invocable<Visitor&, TrackId, TrackView const&>
    void visitTracks(std::span<TrackId const> ids, LoadMode mode, Visitor& visitor) const;

    auto hot() const;
    auto cold() const;
    auto both() const;

  private:
    explicit Reader(lmdb::Database::Reader hotReader, lmdb::Database::Reader coldReader);

    bool shouldUseCursorScan(std::span<TrackId const> ids, LoadMode mode) const;

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

    void validateBothPosition() const;

    lmdb::Database::Reader::Iterator _hotIter;
    lmdb::Database::Reader::Iterator _coldIter;
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

  template<typename Visitor>
    requires std::invocable<Visitor&, TrackId, TrackView const&>
  void TrackStore::Reader::visitTracks(std::span<TrackId const> ids, LoadMode mode, Visitor& visitor) const
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

    for (auto iterator = begin(mode); iterator != EndSentinel{} && requested != ids.end(); ++iterator)
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
   * Record creation and replacement live in TrackWrite.h and take
   * TrackBuilder::PreparedHot/PreparedCold: the record is serialized straight
   * into storage-owned bytes and validated there, so no entry point here
   * accepts caller-supplied record bytes.
   */
  class [[nodiscard]] TrackStore::Writer final
  {
  public:
    /**
     * Get track by ID with specified load mode.
     * @return TrackView, or std::nullopt if the track is missing.
     */
    std::optional<TrackView> get(TrackId id, Reader::LoadMode mode) const;

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
    Writer(lmdb::Database::Writer hotWriter, lmdb::Database::Writer coldWriter);

    lmdb::Database::Writer _hotWriter;
    lmdb::Database::Writer _coldWriter;

    friend class TrackStore;
    friend class detail::TrackWriteAccess;
  };
} // namespace ao::library
