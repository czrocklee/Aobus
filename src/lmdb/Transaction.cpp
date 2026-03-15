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
#include <rs/lmdb/Type.h>

namespace rs::lmdb
{
  ReadTransaction::ReadTransaction(Environment const& env)
  {
    MDB_txn* handle = nullptr;
    throwOnError("mdb_txn_begin", mdb_txn_begin(env._handle.get(), nullptr, MDB_RDONLY, &handle));
    _handle.reset(handle);
  }

  WriteTransaction::WriteTransaction(Environment& env)
  {
    MDB_txn* handle = nullptr;
    throwOnError("mdb_txn_begin", mdb_txn_begin(env._handle.get(), nullptr, 0, &handle));
    _handle.reset(handle);
  }

  // Nested write transaction - child of parent write transaction
  WriteTransaction::WriteTransaction(WriteTransaction& parent) : ReadTransaction()
  {
    MDB_txn* handle = nullptr;
    MDB_env* env = mdb_txn_env(parent._handle.get());
    throwOnError("mdb_txn_begin", mdb_txn_begin(env, parent._handle.get(), 0, &handle));
    _handle.reset(handle);
  }

  void WriteTransaction::commit()
  {
    throwOnError("mdb_txn_commit", mdb_txn_commit(_handle.get()));
    _handle.release(); // Prevent destructor from committing/aborting
  }
}
