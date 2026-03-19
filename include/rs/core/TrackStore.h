// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/TrackLayout.h>
#include <rs/core/Type.h>
#include <rs/lmdb/Database.h>

#include <boost/iterator/transform_iterator.hpp>
#include <optional>
#include <span>
#include <vector>

namespace rs::core
{

  /**
   * TrackStore - Binary storage for tracks using TrackLayout.
   *
   * Uses a simple LMDB database with:
   * - Key: uint32_t track ID
   * - Value: TrackHeader + payload (variable length)
   */
  class TrackStore
  {
  public:
    class Reader;
    class Writer;

    TrackStore(lmdb::WriteTransaction& txn, std::string const& db);

    Reader reader(lmdb::ReadTransaction& txn) const;
    Writer writer(lmdb::WriteTransaction& txn);

  private:
    lmdb::Database _database;
  };

  /**
   * TrackStore::Reader - Read-only access to tracks.
   */
  class TrackStore::Reader
  {
  public:
    class Iterator;

    Iterator begin() const;
    Iterator end() const;

    /**
     * Get a track by ID.
     * @return TrackView pointing to the track data, or std::nullopt if not found
     */
    std::optional<TrackView> get(TrackId id) const;

  private:
    Reader(lmdb::Database::Reader&& reader);

    lmdb::Database::Reader _reader;
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

    bool operator==(Iterator const& other) const;
    Iterator& operator++();
    value_type operator*() const;

  private:
    Iterator(lmdb::Database::Reader::Iterator&& iter);

    lmdb::Database::Reader::Iterator _iter;
    TrackView _view;
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
     * Create a new track from raw binary data.
     * @param data TrackHeader + payload
     * @return Pair of (track ID, TrackView pointing to stored data)
     */
    std::pair<TrackId, TrackView> create(std::span<std::byte const> data);

    /**
     * Update an existing track.
     * @param id Track ID to update
     * @param data New TrackHeader + payload
     * @return TrackView pointing to updated data
     */
    TrackView update(TrackId id, std::span<std::byte const> data);

    bool del(TrackId id);

    /**
     * Get a track by ID.
     */
    std::optional<TrackView> get(TrackId id) const;

  private:
    explicit Writer(lmdb::Database::Writer&& writer);

    lmdb::Database::Writer _writer;
    friend class TrackStore;
  };

} // namespace rs::core
