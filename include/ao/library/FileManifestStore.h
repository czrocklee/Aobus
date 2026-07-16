// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/library/FileManifestView.h>
#include <ao/lmdb/Database.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <string_view>
#include <utility>

namespace ao::library
{
  namespace detail
  {
    class LibraryIdentity;
  }

  class ReadTransaction;
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
    Reader reader(WriteTransaction const& transaction) const;
    Writer writer(WriteTransaction& transaction) const;

  private:
    FileManifestStore(lmdb::Database db, detail::LibraryIdentity const& identity)
      : _db{std::move(db)}, _identity{&identity}
    {
    }

    lmdb::Database _db;
    detail::LibraryIdentity const* _identity;

    friend class MusicLibrary;
  };

  class FileManifestStore::Reader final
  {
  public:
    Result<FileManifestView> get(std::string_view uri) const;

    struct EndSentinel
    {};
    /**
     * Iterator for all manifest entries.
     *
     * Yields entries in strictly increasing lexicographic byte order of the
     * URI key (the manifest database uses LMDB's default memcmp comparator).
     * URI-cursor pagination over the manifest depends on this ordering.
     */
    class Iterator final
    {
    public:
      using value_type = std::pair<std::string_view, FileManifestView>;
      using difference_type = std::ptrdiff_t;
      using iterator_category = std::input_iterator_tag;
      Iterator() = default;
      bool operator==(Iterator const& other) const { return _it == other._it; }
      bool operator==(EndSentinel /*unused*/) const { return _it == lmdb::Database::Reader::Iterator{}; }
      bool operator!=(Iterator const& other) const { return _it != other._it; }

      Iterator& operator++()
      {
        ++_it;
        return *this;
      }
      void operator++(std::int32_t) { ++*this; }

      std::pair<std::string_view, FileManifestView> operator*() const;

    private:
      explicit Iterator(lmdb::Database::Reader::Iterator it)
        : _it{std::move(it)}
      {
      }

      lmdb::Database::Reader::Iterator _it;

      friend class Reader;
    };

    Iterator begin() const;
    EndSentinel end() const { return {}; }

  private:
    explicit Reader(lmdb::Database::Reader reader)
      : _reader{std::move(reader)}
    {
    }

    lmdb::Database::Reader _reader;

    friend class FileManifestStore;
  };

  class [[nodiscard]] FileManifestStore::Writer final
  {
  public:
    Result<FileManifestView> get(std::string_view uri) const;
    Result<> put(std::string_view uri, std::span<std::byte const> payload);
    Result<> remove(std::string_view uri);
    Result<> clear();

  private:
    explicit Writer(lmdb::Database::Writer writer)
      : _writer{std::move(writer)}
    {
    }

    lmdb::Database::Writer _writer;

    friend class FileManifestStore;
  };
} // namespace ao::library
