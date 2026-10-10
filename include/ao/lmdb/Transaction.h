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
  namespace detail
  {
    class DatabaseAccess;
  }

  class WriteTransaction;

  // Read-only transaction
  class [[nodiscard]] ReadTransaction
  {
  public:
    static Result<ReadTransaction> begin(Environment const& env);

    ~ReadTransaction();

    ReadTransaction(ReadTransaction const&) = delete;
    ReadTransaction& operator=(ReadTransaction const&) = delete;

    // Moves transfer readonly native lifetime; the source becomes inactive.
    // Public base transfers involving a WriteTransaction (even finished) are
    // forbidden. Borrowing its read capability is supported; move the writer
    // itself to transfer ownership. Recreate bindings after an owner move.
    ReadTransaction(ReadTransaction&& other) noexcept;
    ReadTransaction& operator=(ReadTransaction&& other) noexcept;
    ReadTransaction(WriteTransaction&& other) = delete;
    ReadTransaction& operator=(WriteTransaction&& other) = delete;

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

    static Result<TxnPtr> create(MDB_env* env, std::uint32_t flags);

    MDB_txn* handle() const noexcept { return _txnPtr.get(); }
    MDB_txn* releaseHandle() noexcept;
    void finish() noexcept;

  private:
    void moveNativeFrom(ReadTransaction& other) noexcept;
    void advanceBindingGeneration() noexcept;

    TxnPtr _txnPtr;
    // Local to this owner object; moves never import another owner's counter.
    std::uint64_t _bindingGeneration = 1;
    ReadFailureMode const _failureMode = ReadFailureMode::Fatal;
    friend class detail::DatabaseAccess;
    friend class WriteTransaction;
  };

  // Read-write transaction (inherits from ReadTransaction for read capabilities).
  // Native writer and admission-mutex ownership stay on the creating thread,
  // including moves and termination; MDB_NOTLS only relaxes readonly affinity.
  class [[nodiscard]] WriteTransaction final : public ReadTransaction
  {
  public:
    static Result<WriteTransaction> begin(Environment& env);

    WriteTransaction(WriteTransaction const&) = delete;
    WriteTransaction& operator=(WriteTransaction const&) = delete;
    WriteTransaction(WriteTransaction&& other) noexcept;
    WriteTransaction& operator=(WriteTransaction&& other) noexcept;
    ~WriteTransaction();

    Result<> commit();

    // Explicitly abort an active transaction. Repeated calls are harmless.
    void abort() noexcept;

    // A finished transaction has no native handle. This includes successful
    // commit, failed commit, explicit abort, and the moved-from state.
    bool isFinished() const noexcept { return !isActive(); }

  private:
    void acquireDatabaseOpenAdmission();
    void releaseDatabaseOpenAdmission() noexcept;

    explicit WriteTransaction(TxnPtr txnPtr)
      : ReadTransaction{std::move(txnPtr), ReadFailureMode::Transaction}
    {
    }

    std::unique_lock<std::mutex> _databaseOpenAdmission;
    friend class detail::DatabaseAccess;
  };
} // namespace ao::lmdb
