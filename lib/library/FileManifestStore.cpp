// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/FileManifestView.h>
#include <ao/library/ReadTransaction.h>
#include <ao/library/WriteTransaction.h>
#include <ao/utility/ByteView.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <expected>
#include <format>
#include <span>
#include <string_view>
#include <utility>

namespace ao::library
{
  namespace
  {
    constexpr std::size_t kMaxUriLength = 500;
    constexpr std::size_t kUriPaddingBufferSize = 504;

    Result<> validateUri(std::string_view uri)
    {
      if (uri.length() > kMaxUriLength)
      {
        return makeError(
          Error::Code::ValueTooLarge,
          std::format("URI length {} exceeds maximum supported length of {} bytes", uri.length(), kMaxUriLength));
      }

      return {};
    }

    std::span<std::byte const> padUri(std::string_view uri, std::span<std::byte> buffer)
    {
      if (uri.size() % 4 == 0)
      {
        return utility::bytes::view(uri);
      }

      std::memcpy(buffer.data(), uri.data(), uri.size());
      size_t const paddedSize = (uri.size() + 3) & ~size_t{3};
      std::memset(buffer.data() + uri.size(), 0, paddedSize - uri.size());
      return buffer.subspan(0, paddedSize);
    }

    class PaddedUriKey final
    {
    public:
      // padUri initializes every byte exposed by _view.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
      explicit PaddedUriKey(std::string_view uri)
        : _view{padUri(uri, _buffer)}
      {
      }

      std::span<std::byte const> view() const { return _view; }

    private:
      std::array<std::byte, kUriPaddingBufferSize> _buffer;
      std::span<std::byte const> _view;
    };
  } // namespace

  FileManifestStore::Reader FileManifestStore::reader(ReadTransaction const& transaction) const
  {
    return Reader{_db.reader(transaction.native(*_identity))};
  }

  FileManifestStore::Reader FileManifestStore::reader(WriteTransaction const& transaction) const
  {
    return Reader{_db.reader(transaction.native(*_identity))};
  }

  FileManifestStore::Writer FileManifestStore::writer(WriteTransaction& transaction) const
  {
    return Writer{_db.writer(transaction.native(*_identity))};
  }

  Result<FileManifestView> FileManifestStore::Reader::get(std::string_view uri) const
  {
    if (auto result = validateUri(uri); !result)
    {
      return std::unexpected{result.error()};
    }

    if (uri.empty())
    {
      return makeError(Error::Code::NotFound, "File manifest entry for empty URI was not found");
    }

    auto const key = PaddedUriKey{uri};

    auto optData = _reader.get(key.view());

    if (!optData)
    {
      return makeError(Error::Code::NotFound, std::format("File manifest entry for URI '{}' was not found", uri));
    }

    if (optData->size() < sizeof(FileManifestHeader))
    {
      return makeError(Error::Code::CorruptData, std::format("File manifest entry for URI '{}' is corrupt", uri));
    }

    return FileManifestView{*optData};
  }

  std::pair<std::string_view, FileManifestView> FileManifestStore::Reader::Iterator::operator*() const
  {
    auto const pair = *_it;
    auto const keyView = utility::bytes::stringView(pair.first);
    auto const actualLength = keyView.find('\0');
    auto const uri = actualLength == std::string_view::npos ? keyView : keyView.substr(0, actualLength);

    return {uri, FileManifestView{pair.second}};
  }

  FileManifestStore::Reader::Iterator FileManifestStore::Reader::begin() const
  {
    return Iterator{_reader.begin()};
  }

  Result<FileManifestView> FileManifestStore::Writer::get(std::string_view uri) const
  {
    if (auto result = validateUri(uri); !result)
    {
      return std::unexpected{result.error()};
    }

    if (uri.empty())
    {
      return makeError(Error::Code::NotFound, "File manifest entry for empty URI was not found");
    }

    auto const key = PaddedUriKey{uri};

    auto optData = _writer.get(key.view());

    if (!optData)
    {
      return makeError(Error::Code::NotFound, std::format("File manifest entry for URI '{}' was not found", uri));
    }

    if (optData->size() < sizeof(FileManifestHeader))
    {
      return makeError(Error::Code::CorruptData, std::format("File manifest entry for URI '{}' is corrupt", uri));
    }

    return FileManifestView{*optData};
  }

  Result<> FileManifestStore::Writer::put(std::string_view uri, std::span<std::byte const> payload)
  {
    if (auto result = validateUri(uri); !result)
    {
      return std::unexpected{result.error()};
    }

    auto const key = PaddedUriKey{uri};

    return _writer.update(key.view(), payload);
  }

  Result<> FileManifestStore::Writer::remove(std::string_view uri)
  {
    if (auto result = validateUri(uri); !result)
    {
      return std::unexpected{result.error()};
    }

    auto const key = PaddedUriKey{uri};

    // Idempotent: a missing row is success. Storage faults throw (see lmdb).
    _writer.del(key.view());
    return {};
  }

  Result<> FileManifestStore::Writer::clear()
  {
    return _writer.clear();
  }
} // namespace ao::library
