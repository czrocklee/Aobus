// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/lmdb/Transaction.h>
#include "ThrowError.h"

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
    _cursorClosed = true;
  }
}
