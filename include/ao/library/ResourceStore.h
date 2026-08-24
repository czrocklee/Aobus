// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/ResourceLayout.h>
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

  /**
   * ResourceStore - the table that resolves a `ResourceId` to a descriptor.
   *
   * The store holds no content. A row names content by digest, and bytes are
   * accepted as that resource only where they hash to it, which is what lets the
   * same image be read from a derived cache or from any media file that carries
   * it. Rows are append-only: nothing counts references and no path deletes one,
   * because a row is 36 bytes and removing one from the middle of a probe chain
   * would strand every digest that probed past it.
   */
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
    using Value = std::pair<ResourceId, ResourceDescriptor>;
    struct EndSentinel
    {};
    class Iterator;

    Iterator begin() const;
    EndSentinel end() const { return {}; }

    // Absence is the only normal miss. Native read faults are fatal and a
    // post-open Resource value that is not a descriptor is an invariant fault.
    std::optional<ResourceDescriptor> get(ResourceId id) const;

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
    std::optional<ResourceDescriptor> get(ResourceId id) const;

    /**
     * @brief Stores the descriptor of content the caller holds.
     *
     * Hashes @p data, so the length written is one the writer counted. That is
     * the evidence a stored length rests on: a counted figure corrects whatever
     * a row held, because bytes that hash to the digest are the truth about it.
     *
     * Fails with `ValueTooLarge` above `UINT32_MAX`, which a stored length
     * cannot represent; nothing is truncated.
     */
    Result<ResourceId> create(std::span<std::byte const> data);

    /**
     * @brief Records a descriptor whose content the caller never saw.
     *
     * A `full` document declares a digest and a length with no bytes to check
     * them against, so the length is a hint. It fills a gap and nothing more:
     * a missing row is created with it, and an existing row keeps the length it
     * already has, because an unverified figure must not beat a counted one. A
     * wrong hint is repaired the first time any writer hashes those bytes.
     */
    Result<ResourceId> getOrCreate(ResourceDescriptor const& descriptor);

    /**
     * @brief Records a descriptor computed from bytes the caller observed.
     *
     * The digest was already calculated before this writer began, while the
     * wrapper preserves the same counted-length evidence as `create(data)`.
     */
    Result<ResourceId> getOrCreate(ObservedResourceDescriptor const& observed);

    /**
     * @brief Deletes one row, which no production path does.
     *
     * `create` probes upward from the digest's initial key and stops at the
     * first empty slot, so an empty slot means "this content is not stored".
     * Removing a row from the middle of a collision chain turns a later entry's
     * path into a hole, and the next `create` for that digest mints a second row
     * the open gate then rejects. No production path calls it; tests reach it to
     * construct exactly that corruption.
     *
     * Returns true if a row was removed, false if the id was absent.
     */
    bool remove(ResourceId id) { return _writer.del(id.raw()); }

    Result<> clear() { return _writer.clear(); }

  private:
    /// What the caller knows about @p descriptor's length.
    enum class LengthEvidence : std::uint8_t
    {
      /// The writer hashed the bytes it counted.
      Counted,
      /// A document said so and nothing checked it.
      Declared,
    };

    Result<ResourceId> store(ResourceDescriptor const& descriptor, LengthEvidence evidence);

    explicit Writer(lmdb::IntegerKeyDatabase::Writer writer)
      : _writer{std::move(writer)}
    {
    }

    lmdb::IntegerKeyDatabase::Writer _writer;
    friend class ResourceStore;
  };
} // namespace ao::library
