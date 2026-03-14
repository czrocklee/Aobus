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

#include <rs/lmdb/Type.h>
#include <rs/utility/TaggedInteger.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace rs::core
{

  /**
   * Tag type for Dictionary IDs - provides type safety.
   */
  struct DictionaryIdTag
  {};
  using DictionaryId = utility::TaggedInteger<std::uint32_t, DictionaryIdTag>;

  /**
   * IDictionary - Abstract interface for string-to-ID lookups.
   *
   * Used by QueryCompiler to resolve string constants to numeric IDs
   * without coupling to LMDB or any specific implementation.
   */
  class IDictionary
  {
  public:
    virtual ~IDictionary() = default;

    /**
     * Look up a string and get its ID.
     *
     * @param str The string to look up
     * @return The ID if found, or 0 if not found
     */
    virtual DictionaryId getStringId(std::string_view str) const = 0;
  };

  /**
   * Dictionary - Bidirectional string-to-ID lookup for track metadata.
   *
   * Provides string interning: strings are stored once and assigned unique IDs.
   * This enables efficient storage and comparison in track queries.
   *
   * Uses two LMDB databases:
   * - dict_read: Key=uint32_t ID, Value=string (for ID → string lookup)
   * - dict_write: Key=string, Value=uint32_t ID (for string → ID lookup)
   *
   * Note: Must be used within a write transaction. The transaction must remain
   * alive for the duration of Dictionary operations.
   */
  class Dictionary : public IDictionary
  {
  public:
    Dictionary(lmdb::MDB readDb, lmdb::MDB writeDb, DictionaryId nextId);

    /**
     * Get or create an ID for the given string.
     * If the string already exists, returns its existing ID.
     * If new, allocates a new ID and stores the mapping.
     *
     * @param txn Write transaction that must remain alive
     * @return The ID associated with the string
     */
    DictionaryId getId(lmdb::Transaction& txn, std::string_view value);

    /**
     * Look up a string by its ID.
     * @param txn Transaction that must remain alive
     * @return The string, or empty string_view if not found
     */
    std::string_view getString(lmdb::Transaction& txn, DictionaryId id) const;

    /**
     * Look up a string and get its ID (for QueryCompiler).
     * @param str The string to look up
     * @return The ID if found, or 0 if not found
     */
    DictionaryId getStringId(std::string_view str) const override;

    /**
     * Get the next available ID (for batch operations).
     */
    DictionaryId nextId() const { return _nextId; }

    /**
     * Set the next available ID.
     */
    void setNextId(DictionaryId id) { _nextId = id; }

  private:
    lmdb::MDB _readDb;
    lmdb::MDB _writeDb;
    DictionaryId _nextId;

    // In-memory cache for read-heavy workloads
    std::unordered_map<std::string, DictionaryId> _stringToIdCache;
    std::unordered_map<DictionaryId, std::string> _idToStringCache;
  };

} // namespace rs::core
