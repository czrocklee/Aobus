// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

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

  class WriteTransaction;
  class MusicLibrary;
  struct MetadataHeader;

  /** Private physical metadata adapter; public callers use MusicLibrary values. */
  class MetadataStore final
  {
  private:
    MetadataStore(lmdb::Database db, detail::LibraryIdentity const& identity)
      : _database{std::move(db)}, _identity{&identity}
    {
    }

    Result<MetadataHeader> load(lmdb::ReadTransaction const& transaction) const;
    Result<> update(WriteTransaction& transaction, MetadataHeader const& header) const;
    std::uint64_t revision(lmdb::ReadTransaction const& transaction) const;
    void persistRevision(lmdb::WriteTransaction& transaction,
                         std::uint64_t candidateRevision,
                         std::uint64_t previousRevision) const;

    lmdb::Database _database;
    detail::LibraryIdentity const* _identity;

    friend class MusicLibrary;
    friend class WriteTransaction;
  };
} // namespace ao::library
