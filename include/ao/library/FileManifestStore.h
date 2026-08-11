// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestView.h>
#include <ao/lmdb/Database.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string_view>
#include <utility>

namespace ao::library
{
  namespace detail
  {
    class LibraryIdentity;
    class PhysicalStoreAccess;
  }

  class ReadTransaction;
  class LibraryWrite;
  class WriteTransaction;
  class MusicLibrary;

  /**
   * FileManifestStore - Manages the mapping between physical file paths and tracks.
   */
  class FileManifestStore final
  {
  public:
    class Reader;
    class Writer;

    Reader reader(ReadTransaction const& transaction) const;
    Reader reader(LibraryWrite const& write) const;
    Reader reader(WriteTransaction const& transaction) const;

  private:
    Writer writer(WriteTransaction& transaction) const;
    FileManifestStore(lmdb::ByteKeyDatabase db, detail::LibraryIdentity const& identity)
      : _db{std::move(db)}, _identity{&identity}
    {
    }

    lmdb::ByteKeyDatabase _db;
    detail::LibraryIdentity const* _identity;

    friend class MusicLibrary;
    friend class TrackWriter;
    friend class WriteTransaction;
    friend class detail::PhysicalStoreAccess;
  };

  class FileManifestStore::Reader final
  {
  public:
    // Absence is the only recoverable miss: returns nullopt if no entry
    // exists for the URI. URI misuse is a caller contract fault; post-open row
    // corruption and native read faults abort through the owned fatal channel.
    std::optional<FileManifestView> get(std::string_view uri) const;

    struct EndSentinel
    {};
    /**
     * Iterator for all manifest entries.
     *
     * Yields entries in strictly increasing lexicographic byte order of the
     * URI key (the manifest database uses LMDB's default memcmp comparator).
     * URI-cursor pagination over the manifest depends on this ordering.
     * A row that violates the open-time storage invariant aborts;
     * iteration never yields a corrupt-row variant or a partial-success result.
     */
    class Iterator final
    {
    public:
      using value_type = std::pair<std::string_view, FileManifestView>;
      using difference_type = std::ptrdiff_t;
      using iterator_category = std::input_iterator_tag;
      Iterator() = default;
      bool operator==(Iterator const& other) const { return _it == other._it; }
      bool operator==(EndSentinel /*unused*/) const { return _it == lmdb::ByteKeyDatabase::Reader::Iterator{}; }
      bool operator!=(Iterator const& other) const { return _it != other._it; }

      Iterator& operator++();
      void operator++(std::int32_t) { ++*this; }

      std::pair<std::string_view, FileManifestView> operator*() const;

    private:
      explicit Iterator(lmdb::ByteKeyDatabase::Reader::Iterator it)
        : _it{std::move(it)}
      {
      }

      lmdb::ByteKeyDatabase::Reader::Iterator _it;

      friend class Reader;
    };

    Iterator begin() const;
    EndSentinel end() const { return {}; }

  private:
    explicit Reader(lmdb::ByteKeyDatabase::Reader reader)
      : _reader{std::move(reader)}
    {
    }

    lmdb::ByteKeyDatabase::Reader _reader;

    friend class FileManifestStore;
  };

  class [[nodiscard]] FileManifestStore::Writer final
  {
  public:
    // Absence is the only recoverable miss: returns nullopt if no entry
    // exists for the URI. URI misuse is a caller contract fault; a read fault
    // aborts the write root and a row breach is an invariant fault.
    std::optional<FileManifestView> get(std::string_view uri) const;
    Result<> put(FileManifestBuilder::Prepared const& prepared);
    // Returns true if a row was removed, false if the URI was absent.
    // Invalid URI is a caller contract fault; storage faults abort the write root.
    bool remove(std::string_view uri);
    // Storage faults use Result.
    Result<> clear();

  private:
    explicit Writer(lmdb::ByteKeyDatabase::Writer writer)
      : _writer{std::move(writer)}
    {
    }

    lmdb::ByteKeyDatabase::Writer _writer;

    friend class FileManifestStore;
  };
} // namespace ao::library
