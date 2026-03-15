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

#include <rs/lmdb/Database.h>
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
   * Used by QueryCompiler to resolve string constants to numeric IDs.
   */
  class IDictionary
  {
  public:
    virtual ~IDictionary() = default;
    [[nodiscard]] virtual DictionaryId getId(std::string_view str) const = 0;
  };

  /**
   * Dictionary - Stores id → string mappings with in-memory string → id index.
   *
   * Uses LMDB for persistent storage (id → string) and builds an in-memory
   * hash map for fast string → id lookups when loaded.
   *
   * Note: Must be used within a transaction. The transaction must remain
   * alive for the duration of Dictionary operations.
   */
  class Dictionary : public IDictionary
  {
  public:
    /**
     * Construct and load existing entries from the database.
     * Builds in-memory string → id index from existing data.
     */
    explicit Dictionary(lmdb::Database& db);

    /**
     * Store a string with the given ID.
     * @param txn Write transaction that must remain alive
     * @param id The ID to associate with the string
     * @param value The string to store
     * @return Pointer to stored data, or nullptr on failure
     */
    void const* store(lmdb::WriteTransaction& txn, DictionaryId id, std::string_view value);

    /**
     * Look up a string by its ID.
     * @param txn Transaction that must remain alive
     * @return The string, or empty string_view if not found
     */
    std::string_view get(lmdb::ReadTransaction& txn, DictionaryId id) const;

    /**
     * Look up an ID by its string.
     * @param str The string to look up
     * @return The ID if found, or 0 if not found
     */
    [[nodiscard]] DictionaryId getId(std::string_view str) const override;

    /**
     * Check if an ID exists.
     * @param txn Transaction that must remain alive
     * @return true if the ID exists
     */
    bool contains(lmdb::ReadTransaction& txn, DictionaryId id) const;

    /**
     * Check if a string exists.
     * @param str The string to look up
     * @return true if the string exists
     */
    bool contains(std::string_view str) const;

  private:
    lmdb::Database& _db;

    // In-memory index: string → id (built on load)
    std::unordered_map<std::string, DictionaryId> _stringToId;
  };

} // namespace rs::core
