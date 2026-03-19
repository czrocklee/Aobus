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

    /**
     * HotProxy - Accessor for hot track data.
     */
    class HotProxy
    {
    public:
      explicit HotProxy(Reader const& reader) : _reader(reader) {}

      std::optional<TrackHotView> get(TrackId id) const;
      Iterator begin(ColdLoadHint hint = ColdLoadHint::Lazy) const;
      Iterator beginEager() const;
      Iterator end() const;

    private:
      Reader const& _reader;
    };

    /**
     * ColdProxy - Accessor for cold track data.
     */
    class ColdProxy
    {
    public:
      explicit ColdProxy(Reader const& reader) : _reader(reader) {}

      std::optional<TrackColdView> get(TrackId id) const;
      Iterator begin() const;
      Iterator end() const;

    private:
      Reader const& _reader;
    };

    Iterator begin(ColdLoadHint hint = ColdLoadHint::Lazy) const;
    Iterator beginEager() const;
    Iterator end() const;

    /**
     * Accessor for hot track data.
     */
    HotProxy hot() const { return HotProxy{*this}; }

    /**
     * Accessor for cold track data.
     */
    ColdProxy cold() const { return ColdProxy{*this}; }

  private:
    Reader(lmdb::Database::Reader&& hotReader, lmdb::Database::Reader&& coldReader);

    lmdb::Database::Reader _hotReader;
    lmdb::Database::Reader _coldReader;
    std::function<std::optional<TrackColdView>(TrackId)> _coldLoader;
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
             std::function<std::optional<TrackColdView>(TrackId)> coldLoader);

    lmdb::Database::Reader::Iterator _hotIter;
    std::optional<lmdb::Database::Reader::Iterator> _coldIter;
    ColdLoadHint _hint = ColdLoadHint::Lazy;
    std::function<std::optional<TrackColdView>(TrackId)> _coldLoader;
    friend class Reader;
    friend class HotProxy;
    friend class ColdProxy;
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
     * @return Pair of (track ID, TrackHotView pointing to stored hot data)
     */
    std::pair<TrackId, TrackHotView> createHotCold(
        std::span<std::byte const> hotData,
        std::span<std::byte const> coldData);

    /**
     * Update hot track data.
     */
    TrackHotView updateHot(TrackId id, std::span<std::byte const> hotData);

    /**
     * Update cold track data.
     */
    TrackColdView updateCold(TrackId id, std::span<std::byte const> coldData);

    /**
     * Delete both hot and cold track data.
     */
    bool delHotCold(TrackId id);

    /**
     * Get hot track by ID.
     */
    std::optional<TrackHotView> getHot(TrackId id) const;

    /**
     * Get cold track by ID.
     */
    std::optional<TrackColdView> getCold(TrackId id) const;

  private:
    explicit Writer(lmdb::Database::Writer&& hotWriter, lmdb::Database::Writer&& coldWriter);

    lmdb::Database::Writer _hotWriter;
    lmdb::Database::Writer _coldWriter;
    friend class TrackStore;
  };

} // namespace rs::core
