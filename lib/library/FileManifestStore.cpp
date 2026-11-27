// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/FileManifestStore.h>

#include "FileManifestValidation.h"
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestView.h>
#include <ao/library/LibraryUri.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ReadTransaction.h>
#include <ao/library/WriteTransaction.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace ao::library
{
  namespace
  {
    void validateUri(std::string_view uri)
    {
      auto parsedRes = LibraryUri::parse(uri);

      AO_EXPECTS(parsedRes, "Invalid file manifest URI");
      AO_EXPECTS(parsedRes->value() == uri, "File manifest URI is not canonical");
    }

    std::pair<std::string_view, FileManifestView> checkedEntry(std::span<std::byte const> key,
                                                               std::span<std::byte const> payload)
    {
      auto const uriRes = validateFileManifestEntry(key, payload);
      AO_INVARIANT(uriRes, "File manifest entry failed validation");

      auto const view = FileManifestView{payload};
      AO_INVARIANT(view.isValid(), "File manifest entry is misaligned");
      return {*uriRes, view};
    }
  } // namespace

  FileManifestStore::Reader FileManifestStore::reader(ReadTransaction const& transaction) const
  {
    return Reader{_db.reader(transaction.native(*_identity))};
  }

  FileManifestStore::Reader FileManifestStore::reader(WriteTransaction const& transaction) const
  {
    return Reader{_db.reader(transaction.native(*_identity))};
  }

  FileManifestStore::Reader FileManifestStore::reader(LibraryWrite const& write) const
  {
    return Reader{_db.reader(write.native(*_identity))};
  }

  FileManifestStore::Writer FileManifestStore::writer(WriteTransaction& transaction) const
  {
    return Writer{_db.writer(transaction.native(*_identity))};
  }

  std::optional<FileManifestView> FileManifestStore::Reader::get(std::string_view uri) const
  {
    validateUri(uri);

    auto const key = detail::PaddedFileManifestKey{uri};

    return _reader.get(key.bytes())
      .transform([&key](auto const payload) { return checkedEntry(key.bytes(), payload).second; });
  }

  FileManifestStore::Reader::Iterator& FileManifestStore::Reader::Iterator::operator++()
  {
    ++_it;
    return *this;
  }

  std::pair<std::string_view, FileManifestView> FileManifestStore::Reader::Iterator::operator*() const
  {
    auto const [key, payload] = *_it;
    return checkedEntry(key, payload);
  }

  FileManifestStore::Reader::Iterator FileManifestStore::Reader::begin() const
  {
    return Iterator{_reader.begin()};
  }

  FileManifestStore::Reader::Iterator FileManifestStore::Reader::lowerBound(std::string_view const uri) const
  {
    validateUri(uri);
    auto const key = detail::PaddedFileManifestKey{uri};
    return Iterator{_reader.lowerBound(key.bytes())};
  }

  std::optional<FileManifestView> FileManifestStore::Writer::get(std::string_view uri) const
  {
    validateUri(uri);

    auto const key = detail::PaddedFileManifestKey{uri};

    return _writer.get(key.bytes())
      .transform([&key](auto const payload) { return checkedEntry(key.bytes(), payload).second; });
  }

  Result<> FileManifestStore::Writer::put(FileManifestBuilder::Prepared const& prepared)
  {
    auto const key = detail::PaddedFileManifestKey{prepared.uri()};
    return _writer.update(key.bytes(), prepared.bytes());
  }

  bool FileManifestStore::Writer::tryRemove(std::string_view uri)
  {
    validateUri(uri);
    auto const key = detail::PaddedFileManifestKey{uri};
    return _writer.tryDelete(key.bytes());
  }

  Result<> FileManifestStore::Writer::clear()
  {
    return _writer.clear();
  }
} // namespace ao::library
