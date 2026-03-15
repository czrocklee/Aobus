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

#include <rs/lmdb/Type.h>

namespace rs::lmdb
{
  // Read-only transaction
  class ReadTransaction
  {
  public:
    ReadTransaction(const Environment& env);
    ReadTransaction(WriteTransaction& parent);  // NOLINT(google-explicit-constructor) - implicit conversion

    ReadTransaction(ReadTransaction&&) = default;

    ~ReadTransaction() noexcept;

    [[nodiscard]] MDB_txn* raw() const noexcept { return _handle; }
    [[nodiscard]] MDB_env* environment() const noexcept { return _handle != nullptr ? mdb_txn_env(_handle) : nullptr; }

  protected:
    explicit ReadTransaction(MDB_txn* handle) noexcept : _handle{handle} {}
    MDB_txn* _handle = nullptr;
    friend class Database;
  };

  // Read-write transaction (inherits from ReadTransaction for read capabilities)
  class WriteTransaction : public ReadTransaction
  {
  public:
    WriteTransaction(Environment& env);
    WriteTransaction(WriteTransaction& parent);

    WriteTransaction(WriteTransaction&&) = default;

    ~WriteTransaction();

    void commit();

  private:
    bool _toCommit = false;
  };
}
