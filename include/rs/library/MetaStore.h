// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/library/Meta.h>
#include <rs/lmdb/Database.h>

#include <optional>

namespace rs::library
{
  class MetaStore final
  {
  public:
    explicit MetaStore(lmdb::Database db)
      : _database{std::move(db)}
    {
    }

    std::optional<MetaHeader> load(lmdb::ReadTransaction& txn) const;
    void create(lmdb::WriteTransaction& txn, MetaHeader const& header);
    void update(lmdb::WriteTransaction& txn, MetaHeader const& header);

  private:
    lmdb::Database _database;
  };
}
