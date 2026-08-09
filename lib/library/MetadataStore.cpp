// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "MetadataStore.h"

#include "lmdb/detail/TransactionFailure.h"
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/WriteTransaction.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/ByteView.h>

#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <utility>

namespace ao::library
{
  Result<MetadataHeader> MetadataStore::load(lmdb::ReadTransaction const& transaction) const
  {
    auto const reader = _database.reader(transaction);
    auto const optBytes = reader.get(kMetadataHeaderRecordId);

    if (!optBytes)
    {
      return makeError(Error::Code::NotFound, "Library metadata header was not found");
    }

    if (optBytes->size() != sizeof(MetadataHeader))
    {
      return makeError(
        Error::Code::CorruptData,
        std::format("Invalid library metadata header size {} (expected {})", optBytes->size(), sizeof(MetadataHeader)));
    }

    auto header = MetadataHeader{};
    std::memcpy(&header, optBytes->data(), sizeof(header));

    if (header.magic != kMetadataMagic || header.libraryVersion != kLibraryVersion || header.flags != 0)
    {
      return makeError(Error::Code::CorruptData, "Library metadata header changed after open validation");
    }

    return header;
  }

  Result<> MetadataStore::update(WriteTransaction& transaction, MetadataHeader const& header) const
  {
    return _database.writer(transaction.native(*_identity))
      .update(kMetadataHeaderRecordId, utility::bytes::view(header));
  }

  std::uint64_t MetadataStore::revision(lmdb::ReadTransaction const& transaction) const
  {
    auto const optBytes = _database.reader(transaction).get(kLibraryRevisionRecordId);

    if (!optBytes)
    {
      return 0;
    }

    AO_INVARIANT(
      optBytes->size() == sizeof(std::uint64_t), "Library revision record size changed after open validation");

    std::uint64_t value = 0;
    std::memcpy(&value, optBytes->data(), sizeof(value));
    AO_INVARIANT(value != 0 && value != std::numeric_limits<std::uint64_t>::max(),
                 "Library revision record contains a reserved value after open validation");
    return value;
  }

  void MetadataStore::persistRevision(lmdb::WriteTransaction& transaction,
                                      std::uint64_t const candidateRevision,
                                      std::uint64_t const previousRevision) const
  {
    AO_INVARIANT(candidateRevision != 0 && candidateRevision != std::numeric_limits<std::uint64_t>::max(),
                 "Candidate library revision is reserved");
    AO_INVARIANT(candidateRevision == previousRevision + 1U, "Candidate library revision is not the exact successor");

    auto writer = _database.writer(transaction);
    auto result = previousRevision == 0
                    ? writer.create(kLibraryRevisionRecordId, utility::bytes::view(candidateRevision))
                    : writer.update(kLibraryRevisionRecordId, utility::bytes::view(candidateRevision));

    if (!result)
    {
      lmdb::detail::throwTransactionFailure(std::move(result.error()));
    }
  }
} // namespace ao::library
