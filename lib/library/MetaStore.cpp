// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/library/MetaStore.h>

#include <ao/Exception.h>
#include <ao/utility/ByteView.h>

#include <cstring>

namespace
{
  constexpr auto kHeaderRecordId = static_cast<std::uint32_t>(ao::library::MetaRecordId::Header);
}

namespace ao::library
{
  std::optional<MetaHeader> MetaStore::load(lmdb::ReadTransaction const& txn) const
  {
    auto const reader = _database.reader(txn);
    auto const bytes = reader.get(kHeaderRecordId);

    if (!bytes)
    {
      return std::nullopt;
    }

    if (bytes->size() != sizeof(MetaHeader))
    {
      AO_THROW_FORMAT(
        ao::Exception, "Invalid library metadata header size {} (expected {})", bytes->size(), sizeof(MetaHeader));
    }

    auto header = MetaHeader{};
    std::memcpy(&header, bytes->data(), sizeof(header));
    return header;
  }

  void MetaStore::create(lmdb::WriteTransaction& txn, MetaHeader const& header)
  {
    _database.writer(txn).create(kHeaderRecordId, utility::bytes::view(header));
  }

  void MetaStore::update(lmdb::WriteTransaction& txn, MetaHeader const& header)
  {
    _database.writer(txn).update(kHeaderRecordId, utility::bytes::view(header));
  }
}
