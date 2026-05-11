// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "detail/ThrowError.h"
#include <ao/lmdb/Transaction.h>

namespace ao::lmdb
{
  auto ReadTransaction::create(::MDB_env* env, ::MDB_txn* parent, unsigned int flags)
  {
    ::MDB_txn* handle = nullptr;
    throwOnError("mdb_txn_begin", ::mdb_txn_begin(env, parent, flags, &handle));
    return std::unique_ptr<::MDB_txn, ReadTransaction::MdbTxnDeleter>{handle};
  }

  ReadTransaction::ReadTransaction(Environment const& env)
    : ReadTransaction{create(env._handle.get(), nullptr, MDB_RDONLY)}
  {
  }

  WriteTransaction::WriteTransaction(Environment& env)
    : ReadTransaction{ReadTransaction::create(env._handle.get(), nullptr, 0)}
  {
  }

  // Nested write transaction - child of parent write transaction
  WriteTransaction::WriteTransaction(WriteTransaction& parent)
    : ReadTransaction{ReadTransaction::create(::mdb_txn_env(parent._handle.get()), parent._handle.get(), 0)}
  {
  }

  void WriteTransaction::commit()
  {
    throwOnError("mdb_txn_commit", ::mdb_txn_commit(_handle.get()));
    std::ignore = _handle.release(); // Prevent destructor from committing/aborting
    _cursorClosed = true;
  }
}
