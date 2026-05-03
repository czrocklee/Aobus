// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <lmdb.h>

#include <ao/lmdb/Environment.h>
#include <memory>

namespace ao::lmdb
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
    struct MdbTxnDeleter
    {
      void operator()(MDB_txn* txn) const noexcept { mdb_txn_abort(txn); }
    };

    ReadTransaction(std::unique_ptr<MDB_txn, MdbTxnDeleter> handle)
      : _handle{std::move(handle)}
    {
    }
    static auto create(MDB_env* env, MDB_txn* parent, unsigned int flags);

    std::unique_ptr<MDB_txn, MdbTxnDeleter> _handle;
    friend class Database;
  };

  // Read-write transaction (inherits from ReadTransaction for read capabilities)
  class WriteTransaction final : public ReadTransaction
  {
  public:
    WriteTransaction(Environment& env);
    // Nested transaction - child of parent write transaction
    explicit WriteTransaction(WriteTransaction& parent);

    WriteTransaction(WriteTransaction const&) = delete;
    WriteTransaction& operator=(WriteTransaction const&) = delete;
    WriteTransaction(WriteTransaction&&) = default;
    WriteTransaction& operator=(WriteTransaction&&) = default;
    ~WriteTransaction() = default;

    void commit();

    // Check if transaction was committed (cursors are now invalid)
    bool isCommitted() const { return _cursorClosed; }

  private:
    bool _cursorClosed = false;
    friend class Database;
  };
}
