// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MetadataStore.h>
#include <ao/library/ReadTransaction.h>
#include <ao/library/WriteTransaction.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/ByteView.h>

#include <cstdint>
#include <cstring>
#include <format>

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
    return header;
  }

  Result<MetadataHeader> MetadataStore::load(ReadTransaction const& transaction) const
  {
    return load(transaction.native(*_identity));
  }

  Result<MetadataHeader> MetadataStore::load(WriteTransaction const& transaction) const
  {
    return load(transaction.native(*_identity));
  }

  Result<> MetadataStore::create(lmdb::WriteTransaction& transaction, MetadataHeader const& header) const
  {
    return _database.writer(transaction).create(kMetadataHeaderRecordId, utility::bytes::view(header));
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

    if (optBytes->size() != sizeof(std::uint64_t))
    {
      throwException<Exception>("Invalid library revision record size");
    }

    std::uint64_t value = 0;
    std::memcpy(&value, optBytes->data(), sizeof(value));
    return value;
  }

  std::uint64_t MetadataStore::revision(ReadTransaction const& transaction) const
  {
    return revision(transaction.native(*_identity));
  }

  std::uint64_t MetadataStore::revision(WriteTransaction const& transaction) const
  {
    return revision(transaction.native(*_identity));
  }

  std::uint64_t MetadataStore::bumpRevision(lmdb::WriteTransaction& transaction) const
  {
    auto const next = revision(transaction) + 1U;
    auto writer = _database.writer(transaction);
    auto result = writer.get(kLibraryRevisionRecordId)
                    ? writer.update(kLibraryRevisionRecordId, utility::bytes::view(next))
                    : writer.create(kLibraryRevisionRecordId, utility::bytes::view(next));

    if (!result)
    {
      throwException<Exception>("Failed to persist library revision: {}", result.error().message);
    }

    return next;
  }
} // namespace ao::library
