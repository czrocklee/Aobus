// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Transaction.h>

#include <cstdint>
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
  struct MetadataHeader;

  class MetadataStore final
  {
  public:
    Result<MetadataHeader> load(ReadTransaction const& transaction) const;
    Result<MetadataHeader> load(WriteTransaction const& transaction) const;
    Result<> update(WriteTransaction& transaction, MetadataHeader const& header) const;

    std::uint64_t revision(ReadTransaction const& transaction) const;
    std::uint64_t revision(WriteTransaction const& transaction) const;

  private:
    MetadataStore(lmdb::Database db, detail::LibraryIdentity const& identity)
      : _database{std::move(db)}, _identity{&identity}
    {
    }

    Result<MetadataHeader> load(lmdb::ReadTransaction const& transaction) const;
    Result<> create(lmdb::WriteTransaction& transaction, MetadataHeader const& header) const;
    std::uint64_t revision(lmdb::ReadTransaction const& transaction) const;
    std::uint64_t bumpRevision(lmdb::WriteTransaction& transaction) const;

    lmdb::Database _database;
    detail::LibraryIdentity const* _identity;

    friend class MusicLibrary;
  };
} // namespace ao::library
