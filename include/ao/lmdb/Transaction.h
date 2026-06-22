// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <cstdint>
#include <memory>
#include <utility>

// LMDB native handle, kept opaque (see Environment.h).
struct MDB_env;
struct MDB_txn;

namespace ao::lmdb
{
  class Environment;
  class WriteTransaction; // Forward declaration

  // Read-only transaction
  class ReadTransaction
  {
  public:
    ReadTransaction(Environment const& env);
    ~ReadTransaction() = default;

    ReadTransaction(ReadTransaction const&) = delete;
    ReadTransaction& operator=(ReadTransaction const&) = delete;

    ReadTransaction(ReadTransaction&&) = default;
    ReadTransaction& operator=(ReadTransaction&&) = default;

  protected:
    struct MdbTxnDeleter
    {
      void operator()(MDB_txn* txn) const noexcept;
    };

    using TxnPtr = std::unique_ptr<MDB_txn, MdbTxnDeleter>;

    ReadTransaction(TxnPtr txnPtr)
      : _txnPtr{std::move(txnPtr)}
    {
    }

    static TxnPtr create(MDB_env* env, MDB_txn* parent, std::uint32_t flags);

    MDB_txn* handle() const noexcept { return _txnPtr.get(); }
    MDB_txn* releaseHandle() noexcept { return _txnPtr.release(); }

  private:
    TxnPtr _txnPtr;
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
    bool committed() const { return _cursorClosed; }

  private:
    bool _cursorClosed = false;
    friend class Database;
  };
} // namespace ao::lmdb
