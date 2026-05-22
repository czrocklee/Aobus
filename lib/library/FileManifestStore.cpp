// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ao/library/FileManifestStore.h"

#include "ao/Exception.h"
#include "ao/library/FileManifestLayout.h"
#include "ao/library/FileManifestView.h"
#include "ao/lmdb/Transaction.h"
#include "ao/utility/ByteView.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace ao::library
{
  namespace
  {
    constexpr std::size_t kMaxUriLength = 500;
    constexpr std::size_t kUriPaddingBufferSize = 504;

    void validateUri(std::string_view uri)
    {
      if (uri.length() > kMaxUriLength)
      {
        ao::throwException<Exception>("URI exceeds maximum supported length of {} bytes", kMaxUriLength);
      }
    }

    std::span<std::byte const> padUri(std::string_view uri, std::span<std::byte> buffer)
    {
      if (uri.size() % 4 == 0)
      {
        return utility::bytes::view(uri);
      }

      std::memcpy(buffer.data(), uri.data(), uri.size());
      size_t const paddedSize = (uri.size() + 3) & ~3;
      std::memset(buffer.data() + uri.size(), 0, paddedSize - uri.size());
      return buffer.subspan(0, paddedSize);
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

  std::optional<FileManifestView> FileManifestStore::Reader::get(std::string_view uri) const
  {
    validateUri(uri);

    auto buffer = std::array<std::byte, kUriPaddingBufferSize>{};
    auto const key = padUri(uri, buffer);

    if (auto const optData = _reader.get(key))
    {
      if (optData->size() < sizeof(FileManifestHeader))
      {
        ao::throwException<Exception>("FileManifestStore: Corrupt entry for URI '{}'", uri);
      }

      return FileManifestView{*optData};
    }

    return std::nullopt;
  }

  std::pair<std::string_view, FileManifestView> FileManifestStore::Reader::Iterator::operator*() const
  {
    auto const pair = *_it;
    auto const keyView = utility::bytes::stringView(pair.first);
    auto const actualLen = keyView.find('\0');
    auto const uri = actualLen == std::string_view::npos ? keyView : keyView.substr(0, actualLen);

    return {uri, FileManifestView{pair.second}};
  }

  FileManifestStore::Reader::Iterator FileManifestStore::Reader::begin() const
  {
    return Iterator{_reader.begin()};
  }

  FileManifestStore::Reader::Iterator FileManifestStore::Reader::end() const
  {
    return Iterator{_reader.end()};
  }

  void FileManifestStore::Writer::put(std::string_view uri, std::span<std::byte const> payload)
  {
    validateUri(uri);

    auto buffer = std::array<std::byte, kUriPaddingBufferSize>{};
    auto const key = padUri(uri, buffer);

    _writer.update(key, payload);
  }

  bool FileManifestStore::Writer::remove(std::string_view uri)
  {
    validateUri(uri);

    auto buffer = std::array<std::byte, kUriPaddingBufferSize>{};
    auto const key = padUri(uri, buffer);

    return _writer.del(key);
  }

  void FileManifestStore::Writer::clear()
  {
    _writer.clear();
  }
} // namespace ao::library
