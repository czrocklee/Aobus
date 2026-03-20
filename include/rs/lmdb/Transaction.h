// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <lmdb.h>

#include <memory>
#include <rs/lmdb/Environment.h>

namespace rs::lmdb
{
  class WriteTransaction; // Forward declaration

  // Read-only transaction
  class ReadTransaction
  {
  public:
    ReadTransaction(Environment const& env);
    ReadTransaction(ReadTransaction&&) = default;
    ReadTransaction& operator=(ReadTransaction&&) = default;

  protected:
    ReadTransaction() = default;
    explicit ReadTransaction(MDB_txn* handle) noexcept : _handle{handle} {}

    struct TxnDeleter
    {
      void operator()(MDB_txn* txn) const { mdb_txn_abort(txn); }
    };
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

    // Check if transaction was committed (cursors are now invalid)
    bool isCommitted() const { return _cursorClosed; }

  private:
    bool _cursorClosed = false;
    friend class Database;
  };
}
