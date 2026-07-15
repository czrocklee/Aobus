// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Transaction.h>

#include <boost/unordered/unordered_flat_set.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>

namespace ao::library
{
  namespace detail
  {
    class LibraryIdentity;
  }

  class WriteTransaction;
  class MusicLibrary;

  /**
   * DictionaryStore - Stores id → string mappings with in-memory string → id index.
   *
   * Uses LMDB for persistent storage (id → string) and builds an in-memory
   * hash map for fast string → id lookups when loaded.
   */
  class DictionaryStore final
  {
  public:
    class Writer;

    DictionaryStore(DictionaryStore&&) = delete;
    DictionaryStore& operator=(DictionaryStore&&) = delete;
    DictionaryStore(DictionaryStore const&) = delete;
    DictionaryStore& operator=(DictionaryStore const&) = delete;
    ~DictionaryStore() = default;

    /**
     * Look up a string by its ID using in-memory index.
     * @param id The dictionary ID
     * @return A borrowed view that remains valid until this store is destroyed.
     *         Published non-empty entries are immutable.
     * @throws std::runtime_error if id is not found
     */
    std::string_view get(DictionaryId id) const;

    /**
     * Look up a string by its ID, returning a default if the ID is invalid.
     * @param id The dictionary ID
     * @param defaultValue Value to return when id is 0 or out of range
     * @return The stored string as a borrowed view, or defaultValue. A stored
     *         non-empty view remains valid until this store is destroyed.
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
     * Look up a committed ID by text.
     * @return The committed ID, or std::nullopt when the text is absent.
     */
    std::optional<DictionaryId> findId(std::string_view str) const;

    /**
     * Check if a string exists.
     * @param str The string to look up
     * @return true if the string exists
     */
    bool contains(std::string_view str) const;

    /**
     * Return the process-local committed dictionary generation.
     * The generation advances once for each published transaction delta.
     */
    std::uint64_t generation() const;

    /**
     * Get the total number of dictionary entries.
     * @return The number of entries
     */
    std::size_t size() const
    {
      auto const lock = std::shared_lock{_mutex};
      return _idToStringStorage.size();
    }

  private:
    DictionaryStore(lmdb::Database db,
                    lmdb::ReadTransaction const& transaction,
                    detail::LibraryIdentity const& identity);

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

      bool operator()(DictionaryId lhs, DictionaryId rhs) const
      {
        return (*storage)[lhs.raw() - 1] == (*storage)[rhs.raw() - 1];
      }

      bool operator()(DictionaryId id, std::string_view str) const { return (*storage)[id.raw() - 1] == str; }

      bool operator()(std::string_view str, DictionaryId id) const { return (*storage)[id.raw() - 1] == str; }
    };

    std::uint64_t bindSymbols(std::span<std::string const> symbols, std::span<DictionaryId> ids) const;

    lmdb::Database _database;
    detail::LibraryIdentity const* _identity;

    // Serializes the complete native-write/publish boundary. LMDB itself has one
    // writer, but this gate begins before the native transaction so allocation and
    // in-process publication share the same order.
    mutable std::mutex _writerMutex;

    // Guards the in-memory indices below. A shared_mutex lets read-mostly
    // lookups run concurrently while committed transaction deltas are published
    // under exclusive ownership.
    mutable std::shared_mutex _mutex;

    // In-memory index: id → string (owner of all strings). A deque keeps element
    // addresses stable across publication, so a string_view returned by get()
    // remains valid until the store is destroyed.
    std::deque<std::string> _idToStringStorage;

    // In-memory index: string_view → id (transparent lookup)
    boost::unordered_flat_set<DictionaryId, DictHash, DictEqual> _stringToId;

    std::uint64_t _generation = 1;

    friend class DictionaryReadContext;
    friend class WriteTransaction;
    friend class MusicLibrary;
  };

  /**
   * Transaction-local dictionary interning port.
   *
   * New mappings are written into the owning LMDB transaction and remain hidden
   * from DictionaryStore readers until WriteTransaction::commit succeeds.
   */
  class [[nodiscard]] DictionaryStore::Writer final
  {
  public:
    ~Writer();

    Writer(Writer const&) = delete;
    Writer& operator=(Writer const&) = delete;
    Writer(Writer&&) noexcept;
    Writer& operator=(Writer&&) noexcept;

    Result<DictionaryId> intern(std::string_view value);

  private:
    Writer(DictionaryStore& dictionary, lmdb::WriteTransaction& transaction);

    void preparePublication();
    void publish() noexcept;
    void rollbackPublication() noexcept;

    struct Impl;
    std::unique_ptr<Impl> _implPtr;

    friend class WriteTransaction;
  };

  /**
   * Reuses immutable dictionary values during one bounded read batch.
   *
   * The cache is not thread-safe. It borrows values from the DictionaryStore
   * and must not outlive that store. Its collision-replacing table has bounded
   * memory; eviction only causes a later store read and never changes results.
   * Empty values are deliberately not cached; a later read simply consults the
   * store again.
   */
  class DictionaryReadCache final
  {
  public:
    explicit DictionaryReadCache(DictionaryStore const& dictionary);

    std::string_view get(DictionaryId id);
    DictionaryStore const& dictionary() const noexcept;

  private:
    struct Entry final
    {
      DictionaryId id{};
      std::string_view value{};
    };

    static constexpr std::size_t kCapacity = 4096;

    DictionaryStore const* _dictionary;
    std::unique_ptr<std::array<Entry, kCapacity>> _entriesPtr;
  };

  /**
   * Bounded synchronous dictionary context for query and format evaluation.
   *
   * The context borrows the store and optional batch cache. It must not outlive
   * either owner and does not provide a multi-call dictionary snapshot.
   */
  class DictionaryReadContext final
  {
  public:
    explicit DictionaryReadContext(DictionaryStore const& dictionary);
    explicit DictionaryReadContext(DictionaryReadCache& cache);

    std::optional<DictionaryId> findId(std::string_view text) const;
    std::uint64_t bind(std::span<std::string const> symbols, std::span<DictionaryId> ids) const;
    std::string_view get(DictionaryId id);
    DictionaryStore const& dictionary() const noexcept;
    std::uint64_t generation() const;

  private:
    DictionaryStore const* _dictionary;
    DictionaryReadCache* _cache = nullptr;
  };
} // namespace ao::library
