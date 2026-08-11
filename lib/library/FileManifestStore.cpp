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

#include <optional>
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

    auto optData = _reader.get(key.bytes());

    if (!optData)
    {
      return std::nullopt;
    }

    auto const validationRes = validateFileManifestEntry(key.bytes(), *optData);
    AO_INVARIANT(validationRes, "File manifest entry failed validation");

    auto view = FileManifestView{*optData};
    AO_INVARIANT(view.isValid(), "File manifest entry is misaligned");

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

    AO_INVARIANT(validationRes, "File manifest iterator encountered invalid data after library validation");

    auto view = FileManifestView{pair.second};
    AO_INVARIANT(view.isValid(), "File manifest iterator encountered a misaligned payload after library validation");

    return {validationRes->uri, view};
  }

  FileManifestStore::Reader::Iterator FileManifestStore::Reader::begin() const
  {
    return Iterator{_reader.begin()};
  }

  std::optional<FileManifestView> FileManifestStore::Writer::get(std::string_view uri) const
  {
    validateUri(uri);

    auto const key = detail::PaddedFileManifestKey{uri};

    auto optData = _writer.get(key.bytes());

    if (!optData)
    {
      return std::nullopt;
    }

    auto const validationRes = validateFileManifestEntry(key.bytes(), *optData);
    AO_INVARIANT(validationRes, "File manifest entry failed validation");

    auto view = FileManifestView{*optData};
    AO_INVARIANT(view.isValid(), "File manifest entry is misaligned");

    return view;
  }

  Result<> FileManifestStore::Writer::put(FileManifestBuilder::Prepared const& prepared)
  {
    auto const key = detail::PaddedFileManifestKey{prepared.uri()};
    return _writer.update(key.bytes(), prepared.bytes());
  }

  bool FileManifestStore::Writer::remove(std::string_view uri)
  {
    validateUri(uri);
    auto const key = detail::PaddedFileManifestKey{uri};
    return _writer.del(key.bytes());
  }

  Result<> FileManifestStore::Writer::clear()
  {
    return _writer.clear();
  }
} // namespace ao::library
