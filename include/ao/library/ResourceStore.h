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
    class PhysicalStoreAccess;
  }

  class ReadTransaction;
  class LibraryWrite;
  class WriteTransaction;
  class MusicLibrary;

  class ResourceStore
  {
  public:
    class Reader;
    class Writer;

    Reader reader(ReadTransaction const& transaction) const;
    Reader reader(LibraryWrite const& write) const;
    Reader reader(WriteTransaction const& transaction) const;

  private:
    Writer writer(WriteTransaction& transaction) const;
    ResourceStore(lmdb::IntegerKeyDatabase db, detail::LibraryIdentity const& identity)
      : _database{std::move(db)}, _identity{&identity}
    {
    }

    lmdb::IntegerKeyDatabase _database;
    detail::LibraryIdentity const* _identity;

    friend class MusicLibrary;
    friend class WriteTransaction;
    friend class detail::PhysicalStoreAccess;
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

    // Absence is the only normal miss. Native read faults are fatal and a
    // post-open empty Resource value is an invariant fault.
    std::optional<std::span<std::byte const>> get(ResourceId id) const;

    ResourceId maxKey() const { return ResourceId{_reader.maxKey()}; }

  private:
    explicit Reader(lmdb::IntegerKeyDatabase::Reader reader)
      : _reader{std::move(reader)}
    {
    }

    lmdb::IntegerKeyDatabase::Reader _reader;
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

    reference operator*() const;

    pointer operator->() const;

    Iterator& operator++();

    void operator++(std::int32_t) { ++*this; }
    bool operator==(EndSentinel /*unused*/) const
    {
      return _iterator == lmdb::IntegerKeyDatabase::Reader::EndSentinel{};
    }

  private:
    explicit Iterator(lmdb::IntegerKeyDatabase::Reader::Iterator iterator)
      : _iterator{std::move(iterator)}
    {
    }

    void refresh() const;

    lmdb::IntegerKeyDatabase::Reader::Iterator _iterator;
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
    std::optional<std::span<std::byte const>> get(ResourceId id) const;
    Result<ResourceId> create(std::span<std::byte const> data);
    // Returns true if a row was removed, false if the id was absent.
    bool remove(ResourceId id) { return _writer.del(id.raw()); }

    Result<> clear() { return _writer.clear(); }

  private:
    explicit Writer(lmdb::IntegerKeyDatabase::Writer writer)
      : _writer{std::move(writer)}
    {
    }

    lmdb::IntegerKeyDatabase::Writer _writer;
    friend class ResourceStore;
  };
} // namespace ao::library
