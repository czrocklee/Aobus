// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/lmdb/Environment.h>

#include <cstdint>
#include <memory>
#include <utility>

// LMDB native handles, kept opaque (see Environment.h).
struct MDB_env;
struct MDB_txn;

namespace ao::lmdb
{
  class WriteTransaction; // Forward declaration

  // Read-only transaction
  class ReadTransaction
  {
  public:
    static Result<ReadTransaction> begin(Environment const& env);

    ~ReadTransaction() = default;

    ReadTransaction(ReadTransaction const&) = delete;
    ReadTransaction& operator=(ReadTransaction const&) = delete;

    ReadTransaction(ReadTransaction&&) = default;
    ReadTransaction& operator=(ReadTransaction&&) = default;

    bool isActive() const noexcept { return handle() != nullptr; }

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

    static Result<TxnPtr> create(MDB_env* env, MDB_txn* parent, std::uint32_t flags);

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
    static Result<WriteTransaction> begin(Environment& env);
    static Result<WriteTransaction> begin(WriteTransaction& parent);

    WriteTransaction(WriteTransaction const&) = delete;
    WriteTransaction& operator=(WriteTransaction const&) = delete;
    WriteTransaction(WriteTransaction&&) = default;
    WriteTransaction& operator=(WriteTransaction&&) = default;
    ~WriteTransaction() = default;

    Result<> commit();

    // Explicitly abort an active transaction. Repeated calls are harmless.
    void abort() noexcept;

    // A finished transaction has no native handle. This includes successful
    // commit, failed commit, explicit abort, and the moved-from state.
    bool isFinished() const noexcept { return !isActive(); }

  private:
    explicit WriteTransaction(TxnPtr txnPtr)
      : ReadTransaction{std::move(txnPtr)}
    {
    }

    friend class Database;
  };
} // namespace ao::lmdb
