/*
 * Copyright (C) 2025 RockStudio
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <rs/core/TrackLayout.h>
#include <rs/lmdb/Database.h>
#include <rs/utility/TaggedInteger.h>

#include <boost/iterator/transform_iterator.hpp>
#include <vector>

namespace rs::core
{

  /**
   * TrackStore - Binary storage for tracks using TrackLayout.
   *
   * Uses a simple LMDB database with:
   * - Key: uint64_t track ID
   * - Value: TrackHeader + payload (variable length)
   */
  class TrackStore
  {
  public:
    struct IdTag
    {};

    using Id = utility::TaggedInteger<std::uint64_t, IdTag>;

    class Reader;
    class Writer;

    TrackStore(lmdb::Environment& env, std::string const& db);

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

    [[nodiscard]] Iterator begin() const;
    [[nodiscard]] Iterator end() const;

    /**
     * Get a track by ID.
     * @return TrackView pointing to the track data, or invalid TrackView if not found
     */
    [[nodiscard]] TrackView operator[](Id id) const;

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
    using value_type = std::pair<Id, TrackView>;

    Iterator() = default;
    Iterator(Iterator const& other) = default;

    [[nodiscard]] bool operator==(Iterator const& other) const;
    Iterator& operator++();
    [[nodiscard]] value_type operator*() const;

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
     * @param data Pointer to TrackHeader + payload
     * @param size Size of the data
     * @return Pair of (track ID, TrackView pointing to stored data)
     */
    [[nodiscard]] std::pair<Id, TrackView> create(void const* data, std::size_t size);

    /**
     * Update an existing track.
     * @param id Track ID to update
     * @param data Pointer to new TrackHeader + payload
     * @param size Size of the data
     * @return TrackView pointing to updated data
     */
    [[nodiscard]] TrackView update(Id id, void const* data, std::size_t size);

    bool del(Id id);

    /**
     * Get a track by ID.
     */
    [[nodiscard]] TrackView operator[](Id id) const;

  private:
    explicit Writer(lmdb::Database::Writer&& writer);

    lmdb::Database::Writer _writer;
    friend class TrackStore;
  };

} // namespace rs::core
