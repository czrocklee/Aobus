// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/lmdb/Transaction.h>

#include "detail/DatabaseOpenAdmissionProbe.h"
#include "detail/ResultError.h"
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/lmdb/Environment.h>

#include <lmdb.h>

#include <cstdint>
#include <expected>
#include <mutex>
#include <semaphore>
#include <thread>
#include <utility>

namespace ao::lmdb
{
  namespace
  {
    std::mutex& databaseOpenMutex()
    {
      static auto mutex = std::mutex{};
      return mutex;
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

    return WriteTransaction{std::move(*txnPtrRes), false};
  }

  Result<WriteTransaction> WriteTransaction::begin(WriteTransaction& parent)
  {
    AO_EXPECTS(parent.isActive(), "Cannot begin a child transaction from a finished parent");

    auto txnPtrRes = create(::mdb_txn_env(parent.handle()), parent.handle(), 0);

    if (!txnPtrRes)
    {
      return std::unexpected{txnPtrRes.error()};
    }

    return WriteTransaction{std::move(*txnPtrRes), true};
  }

  WriteTransaction::WriteTransaction(WriteTransaction&&) noexcept = default;
  WriteTransaction& WriteTransaction::operator=(WriteTransaction&&) noexcept = default;

  WriteTransaction::~WriteTransaction()
  {
    abort();
  }

  void WriteTransaction::acquireDatabaseOpenAdmission()
  {
    AO_EXPECTS(isActive(), "Cannot open a database with a finished write transaction");
    AO_EXPECTS(!_isNested, "Cannot open a database from a nested write transaction");

    if (!_databaseOpenLock.owns_lock())
    {
      auto lock = std::unique_lock{databaseOpenMutex(), std::defer_lock};

      if (!lock.try_lock())
      {
        detail::recordDatabaseOpenAdmissionContention();
        lock.lock();
      }

      _databaseOpenLock = std::move(lock);
    }
  }

  Result<> WriteTransaction::commit()
  {
    AO_EXPECTS(isActive(), "LMDB write transaction is already finished");

    int const rc = ::mdb_txn_commit(releaseHandle());
    _databaseOpenLock = {};
    return resultFromCode("mdb_txn_commit", rc);
  }

  void WriteTransaction::abort() noexcept
  {
    {
      auto transactionPtr = TxnPtr{releaseHandle()};
    }

    _databaseOpenLock = {};
  }
} // namespace ao::lmdb
