// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ao/library/FileManifestView.h"
#include "ao/lmdb/Database.h"

#include <cstddef>
#include <optional>
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

    Reader reader(lmdb::ReadTransaction const& txn) const;
    Writer writer(lmdb::WriteTransaction& txn) const;

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

    std::optional<FileManifestView> get(std::string_view uri) const;

    lmdb::Database::Reader const& databaseReader() const noexcept { return _reader; }

    /**
     * Iterator for all manifest entries.
     */
    class Iterator final
    {
    public:
      Iterator() = default;
      explicit Iterator(lmdb::Database::Reader::Iterator it)
        : _it{std::move(it)}
      {
      }

      bool operator==(Iterator const& other) const { return _it == other._it; }
      bool operator!=(Iterator const& other) const { return _it != other._it; }

      Iterator& operator++()
      {
        ++_it;
        return *this;
      }

      std::pair<std::string_view, FileManifestView> operator*() const;

    private:
      lmdb::Database::Reader::Iterator _it;
    };

    Iterator begin() const;
    Iterator end() const;

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

    void put(std::string_view uri, std::span<std::byte const> payload);
    bool remove(std::string_view uri);
    void clear();

  private:
    lmdb::Database::Writer _writer;
  };
} // namespace ao::library
