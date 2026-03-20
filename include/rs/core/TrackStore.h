// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/TrackView.h>
#include <rs/core/Type.h>
#include <rs/lmdb/Database.h>

#include <boost/iterator/transform_iterator.hpp>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace rs::core
{

  /**
   * TrackStore - Binary storage for tracks using hot/cold separation.
   *
   * Uses two LMDB databases:
   * - tracks_hot: TrackHotHeader + payload (hot fields for fast filtering)
   * - tracks_cold: TrackColdHeader + custom KV + uri (cold fields)
   * - Key: uint32_t track ID (same ID links hot and cold)
   */
  class TrackStore
  {
  public:
    class Reader;
    class Writer;

    TrackStore(lmdb::WriteTransaction& txn, std::string const& hotDb, std::string const& coldDb);

    Reader reader(lmdb::ReadTransaction& txn) const;
    Writer writer(lmdb::WriteTransaction& txn);

  private:
    lmdb::Database _hotDb;
    lmdb::Database _coldDb;
  };

  /**
   * TrackStore::Reader - Read-only access to tracks.
   */
  class TrackStore::Reader
  {
  public:
    class Iterator;

    Iterator begin(ColdLoadHint hint = ColdLoadHint::Lazy) const;
    Iterator beginEager() const;
    Iterator end() const;

    /**
     * Get a track by ID with lazy cold loading.
     * @return TrackView or std::nullopt if not found
     */
    std::optional<TrackView> get(TrackId id) const;

  private:
    Reader(lmdb::Database::Reader&& hotReader, lmdb::Database::Reader&& coldReader);

    lmdb::Database::Reader _hotReader;
    lmdb::Database::Reader _coldReader;
    std::function<std::optional<std::span<std::byte const>>(TrackId)> _coldLoader;
    friend class TrackStore;
  };

  /**
   * TrackStore::Reader::Iterator - Iterator over tracks (using hot database).
   *
   * For Eager mode (ColdLoadHint::Eager), uses two iterators advancing together
   * to avoid random I/O when accessing cold data.
   *
   * For Lazy mode (ColdLoadHint::Lazy), cold data is loaded on demand via
   * the coldLoader callback.
   */
  class TrackStore::Reader::Iterator
  {
  public:
    using value_type = std::pair<TrackId, TrackView>;

    Iterator() = default;
    Iterator(Iterator const& other) = default;

    bool operator==(Iterator const& other) const;
    Iterator& operator++();
    value_type operator*() const;

  private:
    Iterator(lmdb::Database::Reader::Iterator&& hotIter,
             std::optional<lmdb::Database::Reader::Iterator> coldIter,
             ColdLoadHint hint,
             std::function<std::optional<std::span<std::byte const>>(TrackId)> coldLoader);

    lmdb::Database::Reader::Iterator _hotIter;
    std::optional<lmdb::Database::Reader::Iterator> _coldIter;
    ColdLoadHint _hint = ColdLoadHint::Lazy;
    std::function<std::optional<std::span<std::byte const>>(TrackId)> _coldLoader;
    friend class Reader;
  };

  /**
   * TrackStore::Writer - Write access to tracks.
   */
  class TrackStore::Writer
  {
  public:
    Writer() = default;

    /**
     * Create a new track with hot and cold data.
     * @param hotData TrackHotHeader + payload
     * @param coldData TrackColdHeader + custom KV + uri
     * @return Pair of (track ID, TrackView)
     */
    std::pair<TrackId, TrackView> createHotCold(
        std::span<std::byte const> hotData,
        std::span<std::byte const> coldData);

    /**
     * Update hot track data.
     */
    TrackView updateHot(TrackId id, std::span<std::byte const> hotData);

    /**
     * Update cold track data.
     */
    TrackView updateCold(TrackId id, std::span<std::byte const> coldData);

    /**
     * Delete both hot and cold track data.
     */
    bool delHotCold(TrackId id);

    /**
     * Get hot track by ID.
     */
    std::optional<TrackView> getHot(TrackId id) const;

    /**
     * Get cold track by ID.
     */
    std::optional<TrackView> getCold(TrackId id) const;

  private:
    explicit Writer(lmdb::Database::Writer&& hotWriter, lmdb::Database::Writer&& coldWriter);

    lmdb::Database::Writer _hotWriter;
    lmdb::Database::Writer _coldWriter;
    friend class TrackStore;
  };

} // namespace rs::core
