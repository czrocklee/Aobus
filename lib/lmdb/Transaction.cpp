// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/lmdb/Transaction.h>

#include "detail/ResultError.h"
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/lmdb/Environment.h>

#include <lmdb.h>

#include <cstdint>
#include <expected>
#include <utility>

namespace ao::lmdb
{
  void ReadTransaction::MdbTxnDeleter::operator()(MDB_txn* txn) const noexcept
  {
    ::mdb_txn_abort(txn);
  }

  Result<ReadTransaction::TxnPtr> ReadTransaction::create(::MDB_env* env, ::MDB_txn* parent, std::uint32_t flags)
  {
    ::MDB_txn* handle = nullptr;

    if (auto result = resultFromCode("mdb_txn_begin", ::mdb_txn_begin(env, parent, flags, &handle)); !result)
    {
      return std::unexpected{result.error()};
    }

    return TxnPtr{handle};
  }

  Result<ReadTransaction> ReadTransaction::begin(Environment const& env)
  {
    auto txnPtrRes = create(env.handle(), nullptr, MDB_RDONLY);

    if (!txnPtrRes)
    {
      return std::unexpected{txnPtrRes.error()};
    }

    return ReadTransaction{std::move(*txnPtrRes), ReadFailureMode::Fatal};
  }

  Result<WriteTransaction> WriteTransaction::begin(Environment& env)
  {
    auto txnPtrRes = create(env.handle(), nullptr, 0);

    if (!txnPtrRes)
    {
      return std::unexpected{txnPtrRes.error()};
    }

    return WriteTransaction{std::move(*txnPtrRes)};
  }

  Result<WriteTransaction> WriteTransaction::begin(WriteTransaction& parent)
  {
    AO_EXPECTS(parent.isActive(), "Cannot begin a child transaction from a finished parent");

    auto txnPtrRes = create(::mdb_txn_env(parent.handle()), parent.handle(), 0);

    if (!txnPtrRes)
    {
      return std::unexpected{txnPtrRes.error()};
    }

    return WriteTransaction{std::move(*txnPtrRes)};
  }

  Result<> WriteTransaction::commit()
  {
    AO_EXPECTS(isActive(), "LMDB write transaction is already finished");

    int const rc = ::mdb_txn_commit(releaseHandle());
    return resultFromCode("mdb_txn_commit", rc);
  }

  void WriteTransaction::abort() noexcept
  {
    auto transactionPtr = TxnPtr{releaseHandle()};
  }
} // namespace ao::lmdb
