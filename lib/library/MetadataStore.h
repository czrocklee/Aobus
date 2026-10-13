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
  class WriteTransaction;
  class MusicLibrary;
  struct IdSpace;
  struct MetadataHeader;

  /** Private physical metadata adapter; public callers use MusicLibrary values. */
  class MetadataStore final
  {
  private:
    explicit MetadataStore(lmdb::IntegerKeyDatabase db)
      : _database{std::move(db)}
    {
    }

    Result<MetadataHeader> load(lmdb::ReadTransaction const& transaction) const;
    std::uint64_t revision(lmdb::ReadTransaction const& transaction) const;
    void persistRevision(lmdb::WriteTransaction& transaction,
                         std::uint64_t candidateRevision,
                         std::uint64_t previousRevision) const;
    IdSpace idSpace(lmdb::ReadTransaction const& transaction) const;
    void persistIdSpace(lmdb::WriteTransaction& transaction, IdSpace const& idSpace) const;

    lmdb::IntegerKeyDatabase _database;

    friend class MusicLibrary;
    friend class WriteTransaction;
  };
} // namespace ao::library
