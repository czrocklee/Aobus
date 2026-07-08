// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Transaction.h>

#include <boost/unordered/unordered_flat_set.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace ao::library
{
  /**
   * DictionaryStore - Stores id → string mappings with in-memory string → id index.
   *
   * Uses LMDB for persistent storage (id → string) and builds an in-memory
   * hash map for fast string → id lookups when loaded.
   */
  class DictionaryStore final
  {
  public:
    /**
     * Construct and load existing entries from the database.
     * Builds in-memory string → id index from existing data.
     * @param transaction Write transaction for loading existing entries (must remain alive)
     * @param db Database handle
     */
    DictionaryStore(lmdb::Database db, lmdb::ReadTransaction const& transaction);

    DictionaryStore(DictionaryStore&&) = delete;
    DictionaryStore& operator=(DictionaryStore&&) = delete;
    DictionaryStore(DictionaryStore const&) = delete;
    DictionaryStore& operator=(DictionaryStore const&) = delete;
    ~DictionaryStore() = default;

    /**
     * Store a string and auto-generate a unique ID.
     * @param transaction Write transaction that must remain alive
     * @param value The string to store
     * @return The generated ID, or a storage error if the LMDB write fails.
     */
    Result<DictionaryId> put(lmdb::WriteTransaction& transaction, std::string_view value);

    /**
     * Look up a string by its ID using in-memory index.
     * @param id The dictionary ID
     * @return The string
     * @throws std::runtime_error if id is not found
     */
    std::string_view get(DictionaryId id) const;

    /**
     * Look up a string by its ID, returning a default if the ID is invalid.
     * @param id The dictionary ID
     * @param defaultValue Value to return when id is 0 or out of range
     * @return The string or defaultValue
     */
    std::string_view getOrDefault(DictionaryId id, std::string_view defaultValue = {}) const;

    /**
     * Look up an ID by its string.
     * @param str The string to look up
     * @return The ID
     * @throws std::runtime_error if string is not found
     */
    DictionaryId lookupId(std::string_view str) const;

    /**
     * Check if a string exists.
     * @param str The string to look up
     * @return true if the string exists
     */
    bool contains(std::string_view str) const;

    /**
     * Get the total number of dictionary entries.
     * @return The number of entries
     */
    std::size_t size() const
    {
      auto const lock = std::shared_lock{_mutex};
      return _idToStringStorage.size() - _freeIds.size();
    }

    /**
     * Get the ID for a string, or intern it if it doesn't exist.
     * Unlike put(), this does not immediately persist to the database.
     * The ID remains valid for the lifetime of the DictionaryStore.
     */
    DictionaryId getOrIntern(std::string_view str);

  private:
    struct DictHash final
    {
      using is_transparent = void;
      std::deque<std::string> const* storage;

      std::size_t operator()(DictionaryId id) const { return std::hash<std::string_view>{}((*storage)[id.raw() - 1]); }

      std::size_t operator()(std::string_view str) const { return std::hash<std::string_view>{}(str); }
    };

    struct DictEqual final
    {
      using is_transparent = void;
      std::deque<std::string> const* storage;

      bool operator()(DictionaryId lhs, DictionaryId rhs) const { return lhs == rhs; }

      bool operator()(DictionaryId id, std::string_view str) const { return (*storage)[id.raw() - 1] == str; }

      bool operator()(std::string_view str, DictionaryId id) const { return (*storage)[id.raw() - 1] == str; }
    };

    struct PlainDictionaryHash final
    {
      std::size_t operator()(DictionaryId id) const { return std::hash<std::uint32_t>{}(id.raw()); }
    };

    DictionaryId popFreeId()
    {
      if (_freeIds.empty())
      {
        return kInvalidDictionaryId;
      }

      auto id = _freeIds.back();
      _freeIds.pop_back();
      return id;
    }

    lmdb::Database _database;

    // Guards the in-memory indices below. A shared_mutex lets the read-mostly
    // lookups (get/lookupId/contains) run concurrently while put/getOrIntern take
    // exclusive ownership during mutation.
    mutable std::shared_mutex _mutex;

    // In-memory index: id → string (owner of all strings). A deque keeps element
    // addresses stable across growth, so a string_view returned by get() stays
    // valid even after a later put()/getOrIntern() grows the storage.
    std::deque<std::string> _idToStringStorage;

    // In-memory index: string_view → id (transparent lookup)
    boost::unordered_flat_set<DictionaryId, DictHash, DictEqual> _stringToId;

    // Track strings that were reserved but not yet persisted to DB
    boost::unordered_flat_set<DictionaryId, PlainDictionaryHash> _reservedStrings;

    // Track previously freed/skipped IDs to recycle them and prevent gap accumulation
    std::vector<DictionaryId> _freeIds;
  };
} // namespace ao::library
