// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/library/WritableMusicLibrary.h>

#include "WriterSessionLease.h"
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/WriteTransaction.h>

#include <expected>
#include <memory>
#include <tuple>
#include <utility>

namespace ao::library
{
  struct WritableMusicLibrary::Impl final
  {
    MusicLibrary* library = nullptr;
    std::shared_ptr<detail::WriterSessionLease> leasePtr;
  };

  Result<WritableMusicLibrary> WritableMusicLibrary::acquire(MusicLibrary& library)
  {
    auto leaseRes = detail::WriterSessionLease::acquire(library.databasePath());

    if (!leaseRes)
    {
      return std::unexpected{leaseRes.error()};
    }

    // The lease closes the open-to-writer gap. Refresh append-only lookup state
    // from a snapshot acquired after the preceding writer released its lease.
    // readTransaction returns a transaction, not a Result; read failures are
    // fatal and refresh exceptions propagate before writer admission completes.
    std::ignore = library.readTransaction();

    auto implPtr = std::make_unique<Impl>(&library, std::make_shared<detail::WriterSessionLease>(std::move(*leaseRes)));
    return WritableMusicLibrary{std::move(implPtr)};
  }

  WritableMusicLibrary::WritableMusicLibrary(std::unique_ptr<Impl> implPtr)
    : _implPtr{std::move(implPtr)}
  {
  }

  WritableMusicLibrary::~WritableMusicLibrary() = default;
  WritableMusicLibrary::WritableMusicLibrary(WritableMusicLibrary&&) noexcept = default;
  WritableMusicLibrary& WritableMusicLibrary::operator=(WritableMusicLibrary&&) noexcept = default;

  WriteTransaction WritableMusicLibrary::writeTransaction(WriteTransaction::Options options)
  {
    AO_EXPECTS(_implPtr != nullptr);
    return _implPtr->library->beginWriteTransaction(std::move(options), _implPtr->leasePtr);
  }

  MusicLibrary& WritableMusicLibrary::library() const noexcept
  {
    AO_EXPECTS(_implPtr != nullptr);
    return *_implPtr->library;
  }
} // namespace ao::library
