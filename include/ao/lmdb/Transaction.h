// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/lmdb/Environment.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

// LMDB native handles, kept opaque (see Environment.h).
struct MDB_env;
struct MDB_txn;

namespace ao::lmdb
{
  class WriteTransaction; // Forward declaration

  // Read-only transaction
  class [[nodiscard]] ReadTransaction
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

    enum class ReadFailureMode : std::uint8_t
    {
      Fatal,
      Transaction
    };

    ReadTransaction(TxnPtr txnPtr, ReadFailureMode failureMode)
      : _txnPtr{std::move(txnPtr)}, _failureMode{failureMode}
    {
    }

    static Result<TxnPtr> create(MDB_env* env, MDB_txn* parent, std::uint32_t flags);

    MDB_txn* handle() const noexcept { return _txnPtr.get(); }
    MDB_txn* releaseHandle() noexcept { return _txnPtr.release(); }

  private:
    TxnPtr _txnPtr;
    ReadFailureMode _failureMode = ReadFailureMode::Fatal;
    friend class Database;
  };

  // Read-write transaction (inherits from ReadTransaction for read capabilities)
  class [[nodiscard]] WriteTransaction final : public ReadTransaction
  {
  public:
    static Result<WriteTransaction> begin(Environment& env);
    static Result<WriteTransaction> begin(WriteTransaction& parent);

    WriteTransaction(WriteTransaction const&) = delete;
    WriteTransaction& operator=(WriteTransaction const&) = delete;
    WriteTransaction(WriteTransaction&&) noexcept;
    WriteTransaction& operator=(WriteTransaction&&) noexcept;
    ~WriteTransaction();

    Result<> commit();

    // Explicitly abort an active transaction. Repeated calls are harmless.
    void abort() noexcept;

    // A finished transaction has no native handle. This includes successful
    // commit, failed commit, explicit abort, and the moved-from state.
    bool isFinished() const noexcept { return !isActive(); }

  private:
    explicit WriteTransaction(TxnPtr txnPtr, bool isNested)
      : ReadTransaction{std::move(txnPtr), ReadFailureMode::Transaction}, _isNested{isNested}
    {
    }

    void acquireDatabaseOpenAdmission();

    std::unique_lock<std::mutex> _databaseOpenLock;
    bool _isNested = false;

    friend class Database;
  };
} // namespace ao::lmdb
