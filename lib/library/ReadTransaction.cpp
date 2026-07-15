// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "LibraryIdentity.h"
#include <ao/Exception.h>
#include <ao/library/ReadTransaction.h>
#include <ao/lmdb/Transaction.h>

#include <utility>

namespace ao::library
{
  ReadTransaction::ReadTransaction(lmdb::ReadTransaction transaction, detail::LibraryIdentity const& identity) noexcept
    : _transaction{std::move(transaction)}, _identity{&identity}
  {
  }

  lmdb::ReadTransaction const& ReadTransaction::native(detail::LibraryIdentity const& identity) const
  {
    if (_identity != &identity)
    {
      throwException<Exception>("Read transaction belongs to a different MusicLibrary");
    }

    if (!_transaction.isActive())
    {
      throwException<Exception>("Library read transaction is no longer active");
    }

    return _transaction;
  }
} // namespace ao::library
