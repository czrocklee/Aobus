// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/FileManifestStore.h>

#include "FileManifestValidation.h"
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/ExceptionFormat.h>
#include <ao/library/FileManifestView.h>
#include <ao/library/LibraryUri.h>
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
    constexpr std::size_t kUriPaddingBufferSize = (LibraryUri::kMaxLength + 3U) & ~std::size_t{3U};

    Result<> validateUri(std::string_view uri)
    {
      auto parsedRes = LibraryUri::parse(uri);

      if (!parsedRes)
      {
        return std::unexpected{parsedRes.error()};
      }

      if (parsedRes->value() != uri)
      {
        return makeError(Error::Code::InvalidInput, std::format("File manifest URI '{}' is not canonical", uri));
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

    auto const key = PaddedUriKey{uri};

    auto optData = _reader.get(key.view());

    if (!optData)
    {
      return makeError(Error::Code::NotFound, std::format("File manifest entry for URI '{}' was not found", uri));
    }

    if (auto const validationRes = validateFileManifestEntry(key.view(), *optData); !validationRes)
    {
      return std::unexpected{validationRes.error()};
    }

    auto view = FileManifestView{*optData};

    if (!view.isValid())
    {
      return makeError(Error::Code::CorruptData, std::format("File manifest entry for URI '{}' is misaligned", uri));
    }

    return view;
  }

  FileManifestStore::Reader::Iterator& FileManifestStore::Reader::Iterator::operator++()
  {
    ++_it;
    return *this;
  }

  std::pair<std::string_view, FileManifestView> FileManifestStore::Reader::Iterator::operator*() const
  {
    auto const pair = *_it;
    auto validationRes = validateFileManifestEntry(pair.first, pair.second);

    if (!validationRes)
    {
      throwException<Exception>(
        "File manifest iterator encountered invalid data after library validation: {}", validationRes.error().message);
    }

    auto view = FileManifestView{pair.second};

    if (!view.isValid())
    {
      throwException<Exception>("File manifest iterator encountered a misaligned payload after library validation");
    }

    return {validationRes->uri, view};
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

    auto const key = PaddedUriKey{uri};

    auto optData = _writer.get(key.view());

    if (!optData)
    {
      return makeError(Error::Code::NotFound, std::format("File manifest entry for URI '{}' was not found", uri));
    }

    if (auto const validationRes = validateFileManifestEntry(key.view(), *optData); !validationRes)
    {
      return std::unexpected{validationRes.error()};
    }

    auto view = FileManifestView{*optData};

    if (!view.isValid())
    {
      return makeError(Error::Code::CorruptData, std::format("File manifest entry for URI '{}' is misaligned", uri));
    }

    return view;
  }

  Result<> FileManifestStore::Writer::put(std::string_view uri, std::span<std::byte const> payload)
  {
    if (auto result = validateUri(uri); !result)
    {
      return std::unexpected{result.error()};
    }

    auto const key = PaddedUriKey{uri};

    if (auto const result = validateFileManifestEntry(key.view(), payload); !result)
    {
      return std::unexpected{result.error()};
    }

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
