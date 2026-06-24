// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/Error.h>
#include <ao/library/Meta.h>
#include <ao/library/MetaStore.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/ByteView.h>

#include <cstdint>
#include <cstring>
#include <format>
#include <tuple>

namespace ao::library
{
  namespace
  {
    constexpr auto kHeaderRecordId = static_cast<std::uint32_t>(MetaRecordId::Header);
  }

  Result<MetaHeader> MetaStore::load(lmdb::ReadTransaction const& txn) const
  {
    auto const reader = _database.reader(txn);
    auto const optBytes = reader.get(kHeaderRecordId);

    if (!optBytes)
    {
      return makeError(Error::Code::NotFound, "Library metadata header was not found");
    }

    if (optBytes->size() != sizeof(MetaHeader))
    {
      return makeError(
        Error::Code::CorruptData,
        std::format("Invalid library metadata header size {} (expected {})", optBytes->size(), sizeof(MetaHeader)));
    }

    auto header = MetaHeader{};
    std::memcpy(&header, optBytes->data(), sizeof(header));
    return header;
  }

  void MetaStore::create(lmdb::WriteTransaction& txn, MetaHeader const& header)
  {
    std::ignore = _database.writer(txn).create(kHeaderRecordId, utility::bytes::view(header));
  }

  void MetaStore::update(lmdb::WriteTransaction& txn, MetaHeader const& header)
  {
    std::ignore = _database.writer(txn).update(kHeaderRecordId, utility::bytes::view(header));
  }
} // namespace ao::library
