// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/library/Meta.h>
#include <ao/lmdb/Database.h>

#include <optional>

namespace ao::library
{
  class MetaStore final
  {
  public:
    explicit MetaStore(ao::lmdb::Database db)
      : _database{std::move(db)}
    {
    }

    std::optional<MetaHeader> load(ao::lmdb::ReadTransaction const& txn) const;
    void create(ao::lmdb::WriteTransaction& txn, MetaHeader const& header);
    void update(ao::lmdb::WriteTransaction& txn, MetaHeader const& header);

  private:
    ao::lmdb::Database _database;
  };
}
