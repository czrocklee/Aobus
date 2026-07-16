// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "LibraryIdentity.h"
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/WriteTransaction.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>

#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace ao::library
{
  struct WriteTransaction::Impl final
  {
    // A unique_lock is required because commit and failure paths release the
    // process-wide writer gate before this object is destroyed.
    // NOLINTNEXTLINE(aobus-threading-policy)
    Impl(std::unique_lock<std::mutex> writerLock,
         lmdb::WriteTransaction transactionValue,
         DictionaryStore& dictionary,
         detail::LibraryIdentity const& libraryIdentity,
         Options options,
         std::shared_ptr<void const> writerSessionAnchorPtr)
      : writerGate{std::move(writerLock)}
      , transaction{std::move(transactionValue)}
      , dictionaryWriter{dictionary, transaction}
      , identity{&libraryIdentity}
      , optInjectedCommitFailure{std::move(options.optInjectedCommitFailure)}
      , writerSessionAnchorPtr{std::move(writerSessionAnchorPtr)}
    {
    }

    void finishFailure() noexcept
    {
      dictionaryWriter.rollbackPublication();
      transaction.abort();

      if (writerGate.owns_lock())
      {
        writerGate.unlock();
      }

      writerSessionAnchorPtr.reset();
    }

    std::unique_lock<std::mutex> writerGate;
    lmdb::WriteTransaction transaction;
    DictionaryStore::Writer dictionaryWriter;
    detail::LibraryIdentity const* identity;
    std::optional<Error> optInjectedCommitFailure;
    std::shared_ptr<void const> writerSessionAnchorPtr;
  };

  Result<WriteTransaction> WriteTransaction::begin(lmdb::Environment& environment,
                                                   DictionaryStore& dictionary,
                                                   detail::LibraryIdentity const& identity,
                                                   Options options,
                                                   std::shared_ptr<void const> writerSessionAnchorPtr)
  {
    if (dictionary._identity != &identity)
    {
      return makeError(Error::Code::InvalidState, "Dictionary store belongs to a different MusicLibrary");
    }

    // Every library writer acquires the process gate before LMDB's writer lock.
    // Keeping that order fixed prevents two writers from waiting in inversion.
    auto writerGate = std::unique_lock{dictionary._writerMutex};
    auto transaction = lmdb::WriteTransaction::begin(environment);

    if (!transaction)
    {
      return std::unexpected{transaction.error()};
    }

    auto implPtr = std::make_unique<Impl>(std::move(writerGate),
                                          std::move(*transaction),
                                          dictionary,
                                          identity,
                                          std::move(options),
                                          std::move(writerSessionAnchorPtr));
    return WriteTransaction{std::move(implPtr)};
  }

  WriteTransaction::WriteTransaction(std::unique_ptr<Impl> implPtr)
    : _implPtr{std::move(implPtr)}
  {
  }

  WriteTransaction::~WriteTransaction()
  {
    abort();
  }
  WriteTransaction::WriteTransaction(WriteTransaction&&) noexcept = default;
  WriteTransaction& WriteTransaction::operator=(WriteTransaction&& other) noexcept
  {
    if (this != &other)
    {
      abort();
      _implPtr = std::move(other._implPtr);
    }

    return *this;
  }

  DictionaryStore::Writer& WriteTransaction::dictionary()
  {
    if (_implPtr == nullptr || !_implPtr->transaction.isActive())
    {
      throwException<Exception>("Library write transaction is no longer active");
    }

    return _implPtr->dictionaryWriter;
  }

  Result<> WriteTransaction::commit()
  {
    if (_implPtr == nullptr || !_implPtr->transaction.isActive())
    {
      return makeError(Error::Code::InvalidState, "Library write transaction is no longer active");
    }

    auto result = Result<>{};

    try
    {
      _implPtr->dictionaryWriter.preparePublication();

      if (_implPtr->optInjectedCommitFailure)
      {
        _implPtr->transaction.abort();
        result = std::unexpected{std::move(*_implPtr->optInjectedCommitFailure)};
      }
      else
      {
        result = _implPtr->transaction.commit();
      }
    }
    catch (...)
    {
      _implPtr->finishFailure();
      throw;
    }

    if (!result)
    {
      auto error = std::move(result.error());
      _implPtr->finishFailure();
      return std::unexpected{std::move(error)};
    }

    _implPtr->dictionaryWriter.publish();
    _implPtr->writerGate.unlock();
    _implPtr->writerSessionAnchorPtr.reset();
    return {};
  }

  void WriteTransaction::abort() noexcept
  {
    if (_implPtr != nullptr && _implPtr->transaction.isActive())
    {
      _implPtr->finishFailure();
    }
  }

  lmdb::WriteTransaction& WriteTransaction::native(detail::LibraryIdentity const& identity)
  {
    if (_implPtr == nullptr || _implPtr->identity != &identity)
    {
      throwException<Exception>("Write transaction belongs to a different MusicLibrary");
    }

    if (!_implPtr->transaction.isActive())
    {
      throwException<Exception>("Library write transaction is no longer active");
    }

    return _implPtr->transaction;
  }

  lmdb::WriteTransaction const& WriteTransaction::native(detail::LibraryIdentity const& identity) const
  {
    if (_implPtr == nullptr || _implPtr->identity != &identity)
    {
      throwException<Exception>("Write transaction belongs to a different MusicLibrary");
    }

    if (!_implPtr->transaction.isActive())
    {
      throwException<Exception>("Library write transaction is no longer active");
    }

    return _implPtr->transaction;
  }
} // namespace ao::library
