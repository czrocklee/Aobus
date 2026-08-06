// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/library/ReadTransaction.h>

#include "LibraryIdentity.h"
#include <ao/lmdb/Transaction.h>

#include <gsl-lite/gsl-lite.hpp>

#include <utility>

namespace ao::library
{
  ReadTransaction::ReadTransaction(lmdb::ReadTransaction transaction, detail::LibraryIdentity const& identity) noexcept
    : _transaction{std::move(transaction)}, _identity{&identity}
  {
  }

  lmdb::ReadTransaction const& ReadTransaction::native(detail::LibraryIdentity const& identity) const
  {
    gsl_Expects(_identity == &identity && "Read transaction belongs to a different MusicLibrary");
    gsl_Expects(_transaction.isActive() && "Library read transaction is no longer active");

    return _transaction;
  }
} // namespace ao::library
