// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Transaction.h>

#include <utility>

namespace ao::library
{
  struct MetadataHeader;

  class MetadataStore final
  {
  public:
    explicit MetadataStore(lmdb::Database db)
      : _database{std::move(db)}
    {
    }

    Result<MetadataHeader> load(lmdb::ReadTransaction const& transaction) const;
    void create(lmdb::WriteTransaction& transaction, MetadataHeader const& header);
    void update(lmdb::WriteTransaction& transaction, MetadataHeader const& header);

  private:
    lmdb::Database _database;
  };
} // namespace ao::library
