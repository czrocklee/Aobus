// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/library/LibraryMetaStore.h>

#include <rs/Exception.h>
#include <rs/utility/ByteView.h>

#include <cstring>

namespace
{
  constexpr auto kHeaderRecordId = static_cast<std::uint32_t>(rs::library::MetaRecordId::Header);
}

namespace rs::library
{
  std::optional<LibraryMetaHeader> LibraryMetaStore::load(lmdb::ReadTransaction& txn) const
  {
    auto const bytes = _database.reader(txn).get(kHeaderRecordId);

    if (!bytes)
    {
      return std::nullopt;
    }

    if (bytes->size() != sizeof(LibraryMetaHeader))
    {
      RS_THROW_FORMAT(rs::Exception,
                      "Invalid library metadata header size {} (expected {})",
                      bytes->size(),
                      sizeof(LibraryMetaHeader));
    }

    auto header = LibraryMetaHeader{};
    std::memcpy(&header, bytes->data(), sizeof(header));
    return header;
  }

  void LibraryMetaStore::create(lmdb::WriteTransaction& txn, LibraryMetaHeader const& header)
  {
    _database.writer(txn).create(kHeaderRecordId, utility::bytes::view(header));
  }

  void LibraryMetaStore::update(lmdb::WriteTransaction& txn, LibraryMetaHeader const& header)
  {
    _database.writer(txn).update(kHeaderRecordId, utility::bytes::view(header));
  }
}
