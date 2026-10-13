// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/library/LibraryWrite.h>

#include <ao/library/ListWriter.h>
#include <ao/library/TrackWriter.h>
#include <ao/library/WriteTransaction.h>
#include <ao/lmdb/Transaction.h>

namespace ao::library
{
  LibraryWrite::LibraryWrite(WriteTransaction& transaction) noexcept
    : _transaction{&transaction}
  {
  }

  WriteTransaction& LibraryWrite::transaction()
  {
    _transaction->requireOperationActive();
    return *_transaction;
  }

  TrackWriter LibraryWrite::tracks()
  {
    return _transaction->tracks();
  }

  ListWriter LibraryWrite::lists()
  {
    return _transaction->lists();
  }

  lmdb::WriteTransaction const& LibraryWrite::native(detail::LibraryIdentity const& identity) const
  {
    _transaction->requireOperationActive();
    return _transaction->native(identity);
  }
} // namespace ao::library
