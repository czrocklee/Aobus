/*
 * Copyright (C) <year> <name of author>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of  MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <lmdb.h>

#include <memory>
#include <rs/lmdb/Environment.h>

namespace rs::lmdb
{
  class WriteTransaction;  // Forward declaration

  // Read-only transaction
  class ReadTransaction
  {
  public:
    ReadTransaction(const Environment& env);
    ReadTransaction(ReadTransaction&&) = default;
    ReadTransaction& operator=(ReadTransaction&&) = default;

  protected:
    ReadTransaction() = default;
    explicit ReadTransaction(MDB_txn* handle) noexcept : _handle{handle} {}

    struct TxnDeleter { void operator()(MDB_txn* txn) const { mdb_txn_abort(txn); } };
    std::unique_ptr<MDB_txn, TxnDeleter> _handle;
    friend class Database;
  };

  // Read-write transaction (inherits from ReadTransaction for read capabilities)
  class WriteTransaction : public ReadTransaction
  {
  public:
    WriteTransaction(Environment& env);
    // Nested transaction - child of parent write transaction
    explicit WriteTransaction(WriteTransaction& parent);

    WriteTransaction(WriteTransaction const&) = delete;
    WriteTransaction& operator=(WriteTransaction const&) = delete;
    WriteTransaction(WriteTransaction&&) = default;
    WriteTransaction& operator=(WriteTransaction&&) = default;

    void commit();
  };
}
