// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/LibraryMeta.h>
#include <rs/lmdb/Database.h>

#include <optional>

namespace rs::core
{
  class LibraryMetaStore final
  {
  public:
    explicit LibraryMetaStore(lmdb::Database db)
      : _database{std::move(db)}
    {
    }

    std::optional<LibraryMetaHeader> load(lmdb::ReadTransaction& txn) const;
    void create(lmdb::WriteTransaction& txn, LibraryMetaHeader const& header);
    void update(lmdb::WriteTransaction& txn, LibraryMetaHeader const& header);

  private:
    lmdb::Database _database;
  };
}
