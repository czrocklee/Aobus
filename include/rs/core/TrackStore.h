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
     * LoadMode - Controls which data is loaded for each track.
     */
    enum class LoadMode
    {
      Hot,  // Only hot data
      Cold, // Only cold data
      Both  // Both hot and cold
    };

    Iterator begin(LoadMode mode = LoadMode::Both) const;
    Iterator end(LoadMode mode = LoadMode::Both) const;

    /**
     * Get a track by ID.
     * @return TrackView or std::nullopt if not found
     */
    std::optional<TrackView> get(TrackId id, LoadMode mode = LoadMode::Both) const;

  private:
    Reader(lmdb::Database::Reader hotReader, lmdb::Database::Reader coldReader);  // NOLINT(bugprone-easily-swappable-parameters)

    lmdb::Database::Reader _hotReader;
    lmdb::Database::Reader _coldReader;
    friend class TrackStore;
  };

  /**
   * TrackStore::Reader::Iterator - Iterator over tracks.
   */
  class TrackStore::Reader::Iterator
  {
  public:
    using value_type = std::pair<TrackId, TrackView>;

    Iterator() = default;
    Iterator(Iterator const& other) = default;
    // NOLINTBEGIN(cppcoreguidelines-special-member-functions)
    ~Iterator() = default;
    Iterator& operator=(Iterator const&) = default;
    Iterator(Iterator&&) = default;
    Iterator& operator=(Iterator&&) = default;
    // NOLINTEND(cppcoreguidelines-special-member-functions)

    bool operator==(Iterator const& other) const;
    Iterator& operator++();
    value_type operator*() const;

  private:
    Iterator(lmdb::Database::Reader::Iterator&& hotIter,
             lmdb::Database::Reader::Iterator&& coldIter,
             Reader::LoadMode mode);

    std::optional<lmdb::Database::Reader::Iterator> _hotIter;
    std::optional<lmdb::Database::Reader::Iterator> _coldIter;
    Reader::LoadMode _mode = Reader::LoadMode::Both;
    friend class Reader;
  };

  /**
   * TrackStore::Writer - Write access to tracks.
   */
  class TrackStore::Writer
  {
  public:

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
    void updateHot(TrackId id, std::span<std::byte const> hotData);

    /**
     * Update cold track data.
     */
    void updateCold(TrackId id, std::span<std::byte const> coldData);

    /**
     * Delete both hot and cold track data.
     */
    bool remove(TrackId id);

    /**
     * Get track by ID with specified load mode.
     */
    std::optional<TrackView> get(TrackId id, Reader::LoadMode mode) const;

  private:
    explicit Writer(lmdb::Database::Writer&& hotWriter, lmdb::Database::Writer&& coldWriter);

    lmdb::Database::Writer _hotWriter;
    lmdb::Database::Writer _coldWriter;
    friend class TrackStore;
  };

} // namespace rs::core
