// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/Error.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MetadataStore.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/ByteView.h>

#include <cstring>
#include <format>
#include <tuple>

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

  void MetadataStore::create(lmdb::WriteTransaction& transaction, MetadataHeader const& header)
  {
    std::ignore = _database.writer(transaction).create(kMetadataHeaderRecordId, utility::bytes::view(header));
  }

  void MetadataStore::update(lmdb::WriteTransaction& transaction, MetadataHeader const& header)
  {
    std::ignore = _database.writer(transaction).update(kMetadataHeaderRecordId, utility::bytes::view(header));
  }
} // namespace ao::library
