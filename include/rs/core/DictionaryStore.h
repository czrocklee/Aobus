// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/Type.h>
#include <rs/lmdb/Database.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rs::core
{

  /**
   * DictionaryStore - Stores id → string mappings with in-memory string → id index.
   *
   * Uses LMDB for persistent storage (id → string) and builds an in-memory
   * hash map for fast string → id lookups when loaded.
   */
  class DictionaryStore
  {
  public:
    /**
     * Construct and load existing entries from the database.
     * Builds in-memory string → id index from existing data.
     * @param txn Write transaction for loading existing entries (must remain alive)
     * @param db Database name
     */
    explicit DictionaryStore(lmdb::WriteTransaction& txn, std::string const& db);

    /**
     * Store a string and auto-generate a unique ID.
     * @param txn Write transaction that must remain alive
     * @param value The string to store
     * @return The generated ID, or 0 on failure
     */
    DictionaryId put(lmdb::WriteTransaction& txn, std::string_view value);

    /**
     * Look up a string by its ID using in-memory index.
     * @param id The dictionary ID
     * @return The string
     * @throws std::runtime_error if id is not found
     */
    std::string_view get(DictionaryId id) const;

    /**
     * Look up an ID by its string.
     * @param str The string to look up
     * @return The ID
     * @throws std::runtime_error if string is not found
     */
    DictionaryId getId(std::string_view str) const;

    /**
     * Check if a string exists.
     * @param str The string to look up
     * @return true if the string exists
     */
    bool contains(std::string_view str) const;

  private:
    lmdb::Database _database;

    // In-memory index: string_view → id (views into _idToStringStorage)
    std::unordered_map<std::string_view, DictionaryId> _stringToId;

    // In-memory index: id → string (owner of all strings)
    std::vector<std::string> _idToStringStorage;
  };

} // namespace rs::core
