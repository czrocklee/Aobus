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

#include <rs/lmdb/Transaction.h>

namespace rs::lmdb
{
  ReadTransaction::ReadTransaction(const Environment& env)
  {
    throwOnError("mdb_txn_begin", mdb_txn_begin(env.raw(), nullptr, MDB_RDONLY, &_handle));
  }

  ReadTransaction::ReadTransaction(WriteTransaction& parent)
  {
    throwOnError("mdb_txn_begin", mdb_txn_begin(parent.environment(), parent._handle, MDB_RDONLY, &_handle));
  }

  ReadTransaction::~ReadTransaction() noexcept
  {
    if (_handle != nullptr)
    {
      mdb_txn_abort(_handle);
      _handle = nullptr;
    }
  }

  WriteTransaction::WriteTransaction(Environment& env) : ReadTransaction(nullptr)
  {
    MDB_txn* handle = nullptr;
    throwOnError("mdb_txn_begin", mdb_txn_begin(env.raw(), nullptr, 0, &handle));
    _handle = handle;
  }

  WriteTransaction::WriteTransaction(WriteTransaction& parent) : ReadTransaction(nullptr)
  {
    MDB_txn* handle = nullptr;
    throwOnError("mdb_txn_begin", mdb_txn_begin(parent.environment(), parent._handle, 0, &handle));
    _handle = handle;
  }

  WriteTransaction::~WriteTransaction()
  {
    if (_handle != nullptr)
    {
      if (_toCommit)
      {
        mdb_txn_commit(_handle);
      }
      else
      {
        mdb_txn_abort(_handle);
      }
      _handle = nullptr;
    }
  }

  void WriteTransaction::commit()
  {
    _toCommit = true;
  }
}
