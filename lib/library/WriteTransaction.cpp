// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/library/WriteTransaction.h>

#include "LibraryIdentity.h"
#include "MetadataState.h"
#include "MetadataStore.h"
#include "detail/LibraryError.h"
#include "lmdb/detail/TransactionFailure.h"
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListWriter.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackWriter.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <utility>

namespace ao::library
{
  struct WriteTransaction::Impl final
  {
    // A unique_lock is required because commit and failure paths release the
    // process-wide writer gate before this object is destroyed.
    Impl(std::unique_lock<std::mutex> writerLock,
         lmdb::WriteTransaction transactionValue,
         TrackStore& tracksValue,
         ListStore& listsValue,
         ResourceStore& resourcesValue,
         DictionaryStore& dictionary,
         FileManifestStore& manifestValue,
         MetadataStore& metadataValue,
         detail::MetadataState& metadataStateValue,
         detail::MetadataSnapshot metadataSnapshot,
         detail::LibraryIdentity const& libraryIdentity,
         Options options,
         std::shared_ptr<void const> writerSessionAnchorPtr)
      : writerGate{std::move(writerLock)}
      , transaction{std::move(transactionValue)}
      , dictionaryWriter{dictionary, transaction}
      , tracks{&tracksValue}
      , lists{&listsValue}
      , resources{&resourcesValue}
      , manifest{&manifestValue}
      , metadata{&metadataValue}
      , metadataState{&metadataStateValue}
      , candidateHeader{metadataSnapshot.header}
      , previousRevision{metadataSnapshot.revision}
      , candidateRevision{metadataSnapshot.revision + 1U}
      , identity{&libraryIdentity}
      , optInjectedCommitFailure{std::move(options.optInjectedCommitFailure)}
      , writerSessionAnchorPtr{std::move(writerSessionAnchorPtr)}
    {
    }

    void finishFailure() noexcept
    {
      operationActive = false;
      dictionaryWriter.rollbackPublication();
      transaction.abort();

      if (metadataPublicationLock.owns_lock())
      {
        metadataPublicationLock.unlock();
      }

      if (writerGate.owns_lock())
      {
        writerGate.unlock();
      }

      writerSessionAnchorPtr.reset();
    }

    std::unique_lock<std::mutex> writerGate;
    lmdb::WriteTransaction transaction;
    DictionaryStore::Writer dictionaryWriter;
    TrackStore* tracks;
    ListStore* lists;
    ResourceStore* resources;
    FileManifestStore* manifest;
    MetadataStore* metadata;
    detail::MetadataState* metadataState;
    MetadataHeader candidateHeader{};
    std::uint64_t previousRevision = 0;
    std::uint64_t candidateRevision = 0;
    std::unique_lock<std::shared_mutex> metadataPublicationLock;
    std::optional<TrackStore::Writer> optTrackWriter;
    std::optional<ListStore::Writer> optListWriter;
    std::optional<ResourceStore::Writer> optResourceWriter;
    std::optional<FileManifestStore::Writer> optManifestWriter;
    detail::LibraryIdentity const* identity;
    std::optional<Error> optInjectedCommitFailure;
    std::shared_ptr<void const> writerSessionAnchorPtr;
    bool operationActive = false;
    bool metadataHeaderDirty = false;
  };

  Result<WriteTransaction> WriteTransaction::begin(lmdb::Environment& environment,
                                                   TrackStore& tracks,
                                                   ListStore& lists,
                                                   ResourceStore& resources,
                                                   DictionaryStore& dictionary,
                                                   FileManifestStore& manifest,
                                                   MetadataStore& metadata,
                                                   detail::MetadataState& metadataState,
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
    auto transactionRes = lmdb::WriteTransaction::begin(environment);

    if (!transactionRes)
    {
      return std::unexpected{transactionRes.error()};
    }

    auto metadataSnapshot = detail::MetadataSnapshot{};

    try
    {
      auto headerRes = metadata.load(*transactionRes);
      AO_INVARIANT(headerRes, "Library metadata header failed after open validation: {}", headerRes.error().message);
      metadataSnapshot.header = *headerRes;
      metadataSnapshot.revision = metadata.revision(*transactionRes);
    }
    catch (lmdb::detail::TransactionFailure const& failure)
    {
      return std::unexpected{failure.error()};
    }

    AO_INVARIANT(metadataSnapshot.revision < std::numeric_limits<std::uint64_t>::max() - 1U,
                 "Library revision space is exhausted");

    auto implPtr = std::make_unique<Impl>(std::move(writerGate),
                                          std::move(*transactionRes),
                                          tracks,
                                          lists,
                                          resources,
                                          dictionary,
                                          manifest,
                                          metadata,
                                          metadataState,
                                          metadataSnapshot,
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
    AO_EXPECTS(
      (_implPtr != nullptr && _implPtr->transaction.isActive()), "Library write transaction is no longer active");

    return _implPtr->dictionaryWriter;
  }

  TrackWriter WriteTransaction::tracks()
  {
    requireOperationActive();
    return TrackWriter{*this};
  }

  ListWriter WriteTransaction::lists()
  {
    requireOperationActive();
    return ListWriter{*this};
  }

  TrackStore::Writer& WriteTransaction::trackStoreWriter()
  {
    requireOperationActive();

    if (!_implPtr->optTrackWriter)
    {
      _implPtr->optTrackWriter.emplace(_implPtr->tracks->writer(*this));
    }

    return *_implPtr->optTrackWriter;
  }

  ListStore::Writer& WriteTransaction::listStoreWriter()
  {
    requireOperationActive();

    if (!_implPtr->optListWriter)
    {
      _implPtr->optListWriter.emplace(_implPtr->lists->writer(*this));
    }

    return *_implPtr->optListWriter;
  }

  FileManifestStore::Writer& WriteTransaction::manifestStoreWriter()
  {
    requireOperationActive();

    if (!_implPtr->optManifestWriter)
    {
      _implPtr->optManifestWriter.emplace(_implPtr->manifest->writer(*this));
    }

    return *_implPtr->optManifestWriter;
  }

  ResourceStore::Writer& WriteTransaction::resourceStoreWriter(ResourceStore const& resources)
  {
    AO_EXPECTS(
      (_implPtr != nullptr && _implPtr->transaction.isActive()), "Library write transaction is no longer active");
    AO_EXPECTS(_implPtr->resources == &resources, "Resource store belongs to a different MusicLibrary");

    if (!_implPtr->optResourceWriter)
    {
      _implPtr->optResourceWriter.emplace(resources.writer(*this));
    }

    return *_implPtr->optResourceWriter;
  }

  ListStore const& WriteTransaction::listStore() const
  {
    requireOperationActive();
    return *_implPtr->lists;
  }

  ResourceStore const& WriteTransaction::resourceStore() const
  {
    requireOperationActive();
    return *_implPtr->resources;
  }

  Result<> WriteTransaction::restoreLibraryIdentity(std::array<std::byte, 16> const& libraryId)
  {
    requireOperationActive();
    _implPtr->candidateHeader.libraryId = libraryId;
    _implPtr->metadataHeaderDirty = true;
    return {};
  }

  std::uint64_t WriteTransaction::libraryRevision(detail::LibraryIdentity const& identity) const
  {
    AO_EXPECTS(_implPtr != nullptr, "Library write transaction is no longer active");
    AO_EXPECTS(_implPtr->identity == &identity, "Write transaction belongs to a different MusicLibrary");
    return _implPtr->candidateRevision;
  }

  MetadataHeader WriteTransaction::metadataHeader(detail::LibraryIdentity const& identity) const
  {
    AO_EXPECTS(_implPtr != nullptr, "Library write transaction is no longer active");
    AO_EXPECTS(_implPtr->identity == &identity, "Write transaction belongs to a different MusicLibrary");
    return _implPtr->candidateHeader;
  }

  Result<> WriteTransaction::applyBoundary(compat::MoveOnlyFunction<Result<>(LibraryWrite&)> function)
  {
    AO_EXPECTS(
      (_implPtr != nullptr && _implPtr->transaction.isActive()), "Library write transaction is no longer active");

    AO_EXPECTS(!_implPtr->operationActive, "Library write transaction already has an active operation");

    _implPtr->operationActive = true;
    auto write = LibraryWrite{*this};

    try
    {
      auto result = std::invoke(function, write);
      AO_INVARIANT((_implPtr != nullptr && _implPtr->transaction.isActive()),
                   "Library write operation terminated its transaction");

      if (!result)
      {
        abort();
        return std::unexpected{std::move(result.error())};
      }

      _implPtr->operationActive = false;
      return {};
    }
    catch (lmdb::detail::TransactionFailure const& failure)
    {
      AO_INVARIANT((_implPtr != nullptr && _implPtr->transaction.isActive()),
                   "Library write operation terminated its transaction");
      abort();
      return std::unexpected{failure.error()};
    }
    catch (detail::LibraryException const& failure)
    {
      AO_INVARIANT((_implPtr != nullptr && _implPtr->transaction.isActive()),
                   "Library write operation terminated its transaction");
      abort();
      return std::unexpected{failure.error()};
    }
    catch (...)
    {
      AO_INVARIANT((_implPtr != nullptr && _implPtr->transaction.isActive()),
                   "Library write operation terminated its transaction");
      throw;
    }
  }

  void WriteTransaction::requireOperationActive() const
  {
    AO_EXPECTS(
      (_implPtr != nullptr && _implPtr->transaction.isActive()), "Library write transaction is no longer active");
    AO_EXPECTS(_implPtr->operationActive, "Library mutation is outside its apply operation");
  }

  Result<> WriteTransaction::commit()
  {
    AO_EXPECTS(_implPtr != nullptr, "Library write transaction is no longer active");

    AO_EXPECTS(_implPtr->transaction.isActive(), "Library write transaction is no longer active");

    AO_EXPECTS(!_implPtr->operationActive, "Cannot commit during a library write operation");

    auto result = Result<>{};

    try
    {
      _implPtr->dictionaryWriter.preparePublication();
      _implPtr->metadataPublicationLock = std::unique_lock{_implPtr->metadataState->_mutex};
      AO_INVARIANT(_implPtr->metadataState->_revision <= _implPtr->previousRevision,
                   "Published library revision is ahead of the durable write snapshot");

      if (_implPtr->metadataHeaderDirty)
      {
        if (auto headerRes = _implPtr->metadata->update(*this, _implPtr->candidateHeader); !headerRes)
        {
          lmdb::detail::throwTransactionFailure(std::move(headerRes.error()));
        }
      }

      _implPtr->metadata->persistRevision(
        _implPtr->transaction, _implPtr->candidateRevision, _implPtr->previousRevision);

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
    catch (lmdb::detail::TransactionFailure const& failure)
    {
      _implPtr->finishFailure();
      return std::unexpected{failure.error()};
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
    _implPtr->metadataState->_header = _implPtr->candidateHeader;
    _implPtr->metadataState->_revision = _implPtr->candidateRevision;
    _implPtr->metadataPublicationLock.unlock();
    _implPtr->writerGate.unlock();
    _implPtr->writerSessionAnchorPtr.reset();
    return {};
  }

  void WriteTransaction::abort() noexcept
  {
    if (_implPtr != nullptr)
    {
      _implPtr->finishFailure();
    }
  }

  lmdb::WriteTransaction& WriteTransaction::native(detail::LibraryIdentity const& identity)
  {
    AO_EXPECTS((_implPtr != nullptr && _implPtr->identity == &identity),
               "Write transaction belongs to a different MusicLibrary");
    AO_EXPECTS(_implPtr->transaction.isActive(), "Library write transaction is no longer active");

    return _implPtr->transaction;
  }

  lmdb::WriteTransaction const& WriteTransaction::native(detail::LibraryIdentity const& identity) const
  {
    AO_EXPECTS((_implPtr != nullptr && _implPtr->identity == &identity),
               "Write transaction belongs to a different MusicLibrary");
    AO_EXPECTS(_implPtr->transaction.isActive(), "Library write transaction is no longer active");

    return _implPtr->transaction;
  }
} // namespace ao::library
