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
  /**
   * FileManifestStore - Manages the mapping between physical file paths and tracks.
   */
  class FileManifestStore final
  {
  public:
    class Reader;
    class Writer;

    explicit FileManifestStore(lmdb::Database db)
      : _db{std::move(db)}
    {
    }

    Reader reader(lmdb::ReadTransaction const& transaction) const;
    Writer writer(lmdb::WriteTransaction& transaction) const;

  private:
    lmdb::Database _db;
  };

  class FileManifestStore::Reader final
  {
  public:
    explicit Reader(lmdb::Database::Reader reader)
      : _reader{std::move(reader)}
    {
    }

    Result<FileManifestView> get(std::string_view uri) const;

    lmdb::Database::Reader const& databaseReader() const noexcept { return _reader; }

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
      explicit Iterator(lmdb::Database::Reader::Iterator it)
        : _it{std::move(it)}
      {
      }

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
      lmdb::Database::Reader::Iterator _it;
    };

    Iterator begin() const;
    EndSentinel end() const { return {}; }

  private:
    lmdb::Database::Reader _reader;
  };

  class FileManifestStore::Writer final
  {
  public:
    explicit Writer(lmdb::Database::Writer writer)
      : _writer{std::move(writer)}
    {
    }

    Result<FileManifestView> get(std::string_view uri) const;
    Result<> put(std::string_view uri, std::span<std::byte const> payload);
    Result<> remove(std::string_view uri);
    Result<> clear();

  private:
    lmdb::Database::Writer _writer;
  };
} // namespace ao::library
