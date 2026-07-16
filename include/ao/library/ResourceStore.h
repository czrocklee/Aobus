// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/lmdb/Database.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
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

  class ResourceStore
  {
  public:
    class Reader;
    class Writer;

    Reader reader(ReadTransaction const& transaction) const;
    Reader reader(WriteTransaction const& transaction) const;
    Writer writer(WriteTransaction& transaction) const;

  private:
    ResourceStore(lmdb::Database db, detail::LibraryIdentity const& identity)
      : _database{std::move(db)}, _identity{&identity}
    {
    }

    lmdb::Database _database;
    detail::LibraryIdentity const* _identity;

    friend class MusicLibrary;
  };

  class ResourceStore::Reader final
  {
  public:
    using Value = std::pair<ResourceId, std::span<std::byte const>>;
    struct EndSentinel
    {};
    class Iterator;

    Iterator begin() const;
    EndSentinel end() const { return {}; }

    // Absence is the only recoverable miss; storage faults throw (see lmdb).
    std::optional<std::span<std::byte const>> get(ResourceId id) const { return _reader.get(id.raw()); }

    ResourceId maxKey() const { return ResourceId{_reader.maxKey()}; }

  private:
    explicit Reader(lmdb::Database::Reader reader)
      : _reader{std::move(reader)}
    {
    }

    lmdb::Database::Reader _reader;
    friend class ResourceStore;
  };

  class ResourceStore::Reader::Iterator final
  {
  public:
    using iterator_category = std::input_iterator_tag;
    using value_type = Reader::Value;
    using difference_type = std::ptrdiff_t;
    using pointer = value_type const*;
    using reference = value_type const&;

    reference operator*() const
    {
      refresh();
      return _value;
    }

    pointer operator->() const
    {
      refresh();
      return &_value;
    }

    Iterator& operator++()
    {
      ++_iterator;
      return *this;
    }

    void operator++(std::int32_t) { ++*this; }
    bool operator==(EndSentinel /*unused*/) const { return _iterator == lmdb::Database::Reader::EndSentinel{}; }

  private:
    explicit Iterator(lmdb::Database::Reader::Iterator iterator)
      : _iterator{std::move(iterator)}
    {
    }

    void refresh() const { _value = {ResourceId{static_cast<std::uint32_t>(_iterator->first)}, _iterator->second}; }

    lmdb::Database::Reader::Iterator _iterator;
    mutable value_type _value{};

    friend class Reader;
  };

  inline ResourceStore::Reader::Iterator ResourceStore::Reader::begin() const
  {
    return Iterator{_reader.begin()};
  }

  class [[nodiscard]] ResourceStore::Writer
  {
  public:
    std::optional<std::span<std::byte const>> get(ResourceId id) const { return _writer.get(id.raw()); }
    Result<ResourceId> create(std::span<std::byte const> data);
    // Returns true if a row was removed, false if the id was absent.
    bool remove(ResourceId id) { return _writer.del(id.raw()); }

    Result<> clear() { return _writer.clear(); }

  private:
    explicit Writer(lmdb::Database::Writer writer)
      : _writer{std::move(writer)}
    {
    }

    lmdb::Database::Writer _writer;
    friend class ResourceStore;
  };
} // namespace ao::library
