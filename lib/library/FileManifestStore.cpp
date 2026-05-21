// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ao/library/FileManifestStore.h"

#include "ao/Exception.h"
#include "ao/lmdb/Transaction.h"
#include "ao/utility/ByteView.h"

#include <cstring>
#include <optional>
#include <string_view>

namespace ao::library
{
  namespace
  {
    constexpr std::size_t kMaxUriLength = 500;

    void validateUri(std::string_view uri)
    {
      if (uri.length() > kMaxUriLength)
      {
        ao::throwException<Exception>("URI exceeds maximum supported length of {} bytes", kMaxUriLength);
      }
    }
  }

  FileManifestStore::Reader FileManifestStore::reader(lmdb::ReadTransaction const& txn) const
  {
    return Reader{_db.reader(txn)};
  }

  FileManifestStore::Writer FileManifestStore::writer(lmdb::WriteTransaction& txn) const
  {
    return Writer{_db.writer(txn)};
  }

  std::optional<ManifestEntry> FileManifestStore::Reader::get(std::string_view uri) const
  {
    validateUri(uri);

    if (auto const optData = _reader.get(utility::bytes::view(uri)))
    {
      if (optData->size() != sizeof(ManifestEntry))
      {
        ao::throwException<Exception>("FileManifestStore: Corrupt entry for URI '{}'", uri);
      }

      auto entry = ManifestEntry{};
      std::memcpy(&entry, optData->data(), sizeof(ManifestEntry));
      return entry;
    }

    return std::nullopt;
  }

  void FileManifestStore::Writer::put(std::string_view uri, ManifestEntry const& entry)
  {
    validateUri(uri);
    _writer.update(utility::bytes::view(uri), utility::bytes::view(entry));
  }

  bool FileManifestStore::Writer::remove(std::string_view uri)
  {
    validateUri(uri);
    return _writer.del(utility::bytes::view(uri));
  }

  void FileManifestStore::Writer::clear()
  {
    _writer.clear();
  }
} // namespace ao::library
