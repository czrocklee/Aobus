// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Transaction.h>

#include <utility>

namespace ao::library
{
  struct MetaHeader;

  class MetaStore final
  {
  public:
    explicit MetaStore(lmdb::Database db)
      : _database{std::move(db)}
    {
    }

    Result<MetaHeader> load(lmdb::ReadTransaction const& txn) const;
    void create(lmdb::WriteTransaction& txn, MetaHeader const& header);
    void update(lmdb::WriteTransaction& txn, MetaHeader const& header);

  private:
    lmdb::Database _database;
  };
}
