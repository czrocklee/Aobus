// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "detail/ThrowError.h"
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>

#include <lmdb.h>

#include <cstdint>
#include <tuple>

namespace ao::lmdb
{
  void ReadTransaction::MdbTxnDeleter::operator()(MDB_txn* txn) const noexcept
  {
    ::mdb_txn_abort(txn);
  }

  ReadTransaction::TxnPtr ReadTransaction::create(::MDB_env* env, ::MDB_txn* parent, std::uint32_t flags)
  {
    ::MDB_txn* handle = nullptr;
    throwOnError("mdb_txn_begin", ::mdb_txn_begin(env, parent, flags, &handle));
    return TxnPtr{handle};
  }

  ReadTransaction::ReadTransaction(Environment const& env)
    : ReadTransaction{create(env.handle(), nullptr, MDB_RDONLY)}
  {
  }

  WriteTransaction::WriteTransaction(Environment& env)
    : ReadTransaction{ReadTransaction::create(env.handle(), nullptr, 0)}
  {
  }

  // Nested write transaction - child of parent write transaction
  WriteTransaction::WriteTransaction(WriteTransaction& parent)
    : ReadTransaction{ReadTransaction::create(::mdb_txn_env(parent.handle()), parent.handle(), 0)}
  {
  }

  void WriteTransaction::commit()
  {
    throwOnError("mdb_txn_commit", ::mdb_txn_commit(handle()));
    std::ignore = releaseHandle(); // Prevent destructor from committing/aborting
    _cursorClosed = true;
  }
}
