// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/lmdb/Transaction.h>

#include "detail/DatabaseOpenAdmissionProbe.h"
#include "detail/ResultError.h"
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/lmdb/Environment.h>

#include <lmdb.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <semaphore>
#include <thread>
#include <utility>

namespace ao::lmdb
{
  namespace
  {
    std::mutex& databaseOpenAdmission()
    {
      // Only writers open DBIs, after acquiring their native writer lock.
      static auto admission = std::mutex{};
      return admission;
    }

    detail::DatabaseOpenAdmissionProbe*& databaseOpenAdmissionProbe() noexcept
    {
      thread_local auto* probe = static_cast<detail::DatabaseOpenAdmissionProbe*>(nullptr);
      return probe;
    }
  } // namespace

  namespace detail
  {
    DatabaseOpenAdmissionProbe::DatabaseOpenAdmissionProbe(std::binary_semaphore& contentionSignal)
      : _contentionSignal{contentionSignal}, _ownerThreadId{std::this_thread::get_id()}
    {
      AO_EXPECTS(
        databaseOpenAdmissionProbe() == nullptr, "A database-open admission probe is already active on this thread");
      databaseOpenAdmissionProbe() = this;
    }

    DatabaseOpenAdmissionProbe::~DatabaseOpenAdmissionProbe()
    {
      AO_INVARIANT(
        std::this_thread::get_id() == _ownerThreadId, "Database-open admission probe left its owning thread");
      AO_INVARIANT(databaseOpenAdmissionProbe() == this, "Database-open admission probe ownership was replaced");
      databaseOpenAdmissionProbe() = nullptr;
    }

    void recordDatabaseOpenAdmissionContention() noexcept
    {
      auto* const probe = databaseOpenAdmissionProbe();

      if (probe == nullptr || probe->_observed)
      {
        return;
      }

      probe->_observed = true;
      probe->_contentionSignal.release();
    }
  } // namespace detail

  ReadTransaction::~ReadTransaction()
  {
    finish();
  }

  ReadTransaction::ReadTransaction(ReadTransaction&& other) noexcept
  {
    AO_EXPECTS(
      other._failureMode == ReadFailureMode::Fatal, "Cannot transfer writer ownership through ReadTransaction");
    moveNativeFrom(other);
  }

  ReadTransaction& ReadTransaction::operator=(ReadTransaction&& other) noexcept
  {
    AO_EXPECTS(_failureMode == ReadFailureMode::Fatal && other._failureMode == ReadFailureMode::Fatal,
               "Cannot transfer writer ownership through ReadTransaction");

    if (this != &other)
    {
      moveNativeFrom(other);
    }

    return *this;
  }

  void ReadTransaction::moveNativeFrom(ReadTransaction& other) noexcept
  {
    // This private path also serves complete writer moves, never base slicing.
    AO_INVARIANT(_failureMode == other._failureMode);
    advanceBindingGeneration();
    finish();
    other.advanceBindingGeneration();
    _txnPtr = std::move(other._txnPtr);
    _openCursorCountPtr = std::move(other._openCursorCountPtr);
  }

  MDB_txn* ReadTransaction::releaseHandle() noexcept
  {
    if (_txnPtr != nullptr)
    {
      advanceBindingGeneration();
    }

    return _txnPtr.release();
  }

  void ReadTransaction::finish() noexcept
  {
    requireNoOpenCursors();
    auto transactionPtr = TxnPtr{releaseHandle()};
  }

  void ReadTransaction::MdbTxnDeleter::operator()(MDB_txn* txn) const noexcept
  {
    ::mdb_txn_abort(txn);
  }

  Result<ReadTransaction::TxnPtr> ReadTransaction::create(::MDB_env* env, std::uint32_t flags)
  {
    ::MDB_txn* handle = nullptr;

    if (auto res = resultFromCode("mdb_txn_begin", ::mdb_txn_begin(env, nullptr, flags, &handle)); !res)
    {
      return std::unexpected{res.error()};
    }

    return TxnPtr{handle};
  }

  Result<ReadTransaction> ReadTransaction::begin(Environment const& env)
  {
    auto txnPtrRes = create(env.handle(), MDB_RDONLY);

    if (!txnPtrRes)
    {
      return std::unexpected{txnPtrRes.error()};
    }

    auto transaction = ReadTransaction{std::move(*txnPtrRes), ReadFailureMode::Fatal};
    transaction._openCursorCountPtr = std::make_unique<std::size_t>(0);
    return transaction;
  }

  void ReadTransaction::requireNoOpenCursors() const noexcept
  {
    AO_EXPECTS(_openCursorCountPtr == nullptr || *_openCursorCountPtr == 0,
               "Read-only LMDB transaction ended while one of its cursors remains open");
  }

  Result<WriteTransaction> WriteTransaction::begin(Environment& env)
  {
    auto txnPtrRes = create(env.handle(), 0);

    if (!txnPtrRes)
    {
      return std::unexpected{txnPtrRes.error()};
    }

    return WriteTransaction{std::move(*txnPtrRes)};
  }

  WriteTransaction::WriteTransaction(WriteTransaction&& other) noexcept
    : ReadTransaction{TxnPtr{}, ReadFailureMode::Transaction}
    , _databaseOpenAdmission{std::move(other._databaseOpenAdmission)}
  {
    AO_EXPECTS(!other.isActive() || other.isOwnerThread(), "LMDB write transaction moved off its creating thread");
    _ownerThreadId = other._ownerThreadId;
    moveNativeFrom(other);
  }

  WriteTransaction& WriteTransaction::operator=(WriteTransaction&& other) noexcept
  {
    if (this != &other)
    {
      AO_EXPECTS(!other.isActive() || other.isOwnerThread(), "LMDB write transaction moved off its creating thread");
      // End the replaced native writer before releasing its admission.
      abort();
      moveNativeFrom(other);
      _databaseOpenAdmission = std::move(other._databaseOpenAdmission);
      _ownerThreadId = other._ownerThreadId;
    }

    return *this;
  }

  WriteTransaction::~WriteTransaction()
  {
    abort();
  }

  bool ReadTransaction::isOwnerThread() const noexcept
  {
    return _failureMode == ReadFailureMode::Fatal || std::this_thread::get_id() == _ownerThreadId;
  }

  void WriteTransaction::acquireDatabaseOpenAdmission()
  {
    AO_EXPECTS(isActive(), "Cannot open a database with a finished transaction");
    AO_EXPECTS(isOwnerThread(), "LMDB write transaction used off its creating thread");

    if (!_databaseOpenAdmission.owns_lock())
    {
      _databaseOpenAdmission = std::unique_lock{databaseOpenAdmission(), std::defer_lock};

      if (!_databaseOpenAdmission.try_lock())
      {
        detail::recordDatabaseOpenAdmissionContention();
        _databaseOpenAdmission.lock();
      }
    }
  }

  void WriteTransaction::releaseDatabaseOpenAdmission() noexcept
  {
    // This checks wrapper ownership, not completion of a native call after releaseHandle().
    AO_INVARIANT(_txnPtr == nullptr, "Cannot release database-open admission while owning a native transaction");
    _databaseOpenAdmission = {};
  }

  void ReadTransaction::advanceBindingGeneration() noexcept
  {
    // Allocate identities across owner lifetimes; this does not synchronize transaction access.
    static auto lastGeneration = std::atomic<std::uint64_t>{0};
    _bindingGeneration = lastGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
    AO_INVARIANT(_bindingGeneration != 0, "LMDB transaction binding generation exhausted");
  }

  Result<> WriteTransaction::commit()
  {
    AO_EXPECTS(isActive(), "LMDB write transaction is already finished");
    AO_EXPECTS(isOwnerThread(), "LMDB write transaction committed off its creating thread");

    int const rc = ::mdb_txn_commit(releaseHandle());
    releaseDatabaseOpenAdmission();
    return resultFromCode("mdb_txn_commit", rc);
  }

  void WriteTransaction::abort() noexcept
  {
    AO_EXPECTS(!isActive() || isOwnerThread(), "LMDB write transaction aborted off its creating thread");
    finish();
    releaseDatabaseOpenAdmission();
  }
} // namespace ao::lmdb
