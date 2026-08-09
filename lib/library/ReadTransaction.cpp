// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/library/ReadTransaction.h>

#include "LibraryIdentity.h"
#include <ao/Contract.h>
#include <ao/library/MetadataLayout.h>
#include <ao/lmdb/Transaction.h>

#include <cstdint>
#include <utility>

namespace ao::library
{
  ReadTransaction::ReadTransaction(lmdb::ReadTransaction transaction,
                                   detail::LibraryIdentity const& identity,
                                   MetadataHeader metadataHeader,
                                   std::uint64_t const libraryRevision) noexcept
    : _transaction{std::move(transaction)}
    , _identity{&identity}
    , _metadataHeader{metadataHeader}
    , _libraryRevision{libraryRevision}
  {
  }

  lmdb::ReadTransaction const& ReadTransaction::native(detail::LibraryIdentity const& identity) const
  {
    AO_EXPECTS(_identity == &identity, "Read transaction belongs to a different MusicLibrary");
    AO_EXPECTS(_transaction.isActive(), "Library read transaction is no longer active");

    return _transaction;
  }
} // namespace ao::library
