// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "WriterSessionLease.h"
#include <ao/Error.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/library/WriteTransaction.h>

#include <gsl-lite/gsl-lite.hpp>

#include <expected>
#include <memory>
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
    auto leaseResult = detail::WriterSessionLease::acquire(library.databasePath());

    if (!leaseResult)
    {
      return std::unexpected{leaseResult.error()};
    }

    auto implPtr =
      std::make_unique<Impl>(&library, std::make_shared<detail::WriterSessionLease>(std::move(*leaseResult)));
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
    gsl_Expects(_implPtr != nullptr);
    return _implPtr->library->beginWriteTransaction(std::move(options), _implPtr->leasePtr);
  }

  MusicLibrary& WritableMusicLibrary::library() const noexcept
  {
    gsl_Expects(_implPtr != nullptr);
    return *_implPtr->library;
  }
} // namespace ao::library
