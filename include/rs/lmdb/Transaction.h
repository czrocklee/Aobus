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
  // NOLINTBEGIN(cppcoreguidelines-special-member-functions)
  class ReadTransaction
  {
  public:
    ReadTransaction(Environment const& env);
    ReadTransaction(ReadTransaction&&) = default;
    ReadTransaction& operator=(ReadTransaction&&) = default;

  protected:
    ReadTransaction() = default;
    explicit ReadTransaction(MDB_txn* handle) noexcept : _handle{handle} {}

    std::unique_ptr<MDB_txn, decltype([](auto* txn) { mdb_txn_abort(txn); })> _handle;
    friend class Database;
  };
  // NOLINTEND(cppcoreguidelines-special-member-functions)

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
    ~WriteTransaction() = default;

    void commit();

    // Check if transaction was committed (cursors are now invalid)
    bool isCommitted() const { return _cursorClosed; }

  private:
    bool _cursorClosed = false;
    friend class Database;
  };
}
