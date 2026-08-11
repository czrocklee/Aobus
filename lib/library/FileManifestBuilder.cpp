// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/FileManifestBuilder.h>

#include "FileManifestValidation.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestView.h>
#include <ao/library/LibraryUri.h>
#include <ao/utility/Hash128.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::library
{
  FileManifestBuilder FileManifestBuilder::makeEmpty()
  {
    return FileManifestBuilder{};
  }

  FileManifestBuilder FileManifestBuilder::fromView(FileManifestView const& view)
  {
    AO_EXPECTS(view.isValid(), "Cannot prepare a file manifest from an invalid view");

    auto builder = FileManifestBuilder{};
    builder.trackId(view.trackId())
      .fileSize(view.fileSize())
      .mtime(view.mtime())
      .audioPayloadLength(view.audioPayloadLength())
      .audioSignature(view.audioSignature())
      .status(view.status());
    return builder;
  }

  FileManifestBuilder& FileManifestBuilder::trackId(TrackId val)
  {
    _header.trackId = val;
    return *this;
  }

  FileManifestBuilder& FileManifestBuilder::fileSize(std::uint64_t val)
  {
    _header.fileSize(val);
    return *this;
  }

  FileManifestBuilder& FileManifestBuilder::mtime(std::uint64_t val)
  {
    _header.mtime(val);
    return *this;
  }

  FileManifestBuilder& FileManifestBuilder::audioPayloadLength(std::uint64_t val)
  {
    _header.audioPayloadLength(val);
    return *this;
  }

  FileManifestBuilder& FileManifestBuilder::audioSignature(utility::Hash128 val)
  {
    _header.audioSignature(val);
    return *this;
  }

  FileManifestBuilder& FileManifestBuilder::status(FileStatus val)
  {
    _header.status = val;
    return *this;
  }

  FileManifestBuilder::Prepared::Prepared(LibraryUri uri, FileManifestHeader header)
    : _uri{std::move(uri)}, _header{header}
  {
  }

  Result<FileManifestBuilder::Prepared> FileManifestBuilder::prepare(std::string_view uri) const
  {
    auto uriRes = LibraryUri::parse(uri);

    if (!uriRes)
    {
      return std::unexpected{uriRes.error()};
    }

    return prepare(std::move(*uriRes));
  }

  Result<FileManifestBuilder::Prepared> FileManifestBuilder::prepare(LibraryUri uri) const
  {
    auto const key = detail::PaddedFileManifestKey{uri.value()};
    auto const payload = std::as_bytes(std::span{&_header, std::size_t{1}});

    if (auto validationRes = validateFileManifestEntry(key.bytes(), payload); !validationRes)
    {
      return std::unexpected{validationRes.error()};
    }

    return Prepared{std::move(uri), _header};
  }

  std::vector<std::byte> FileManifestBuilder::serialize() const
  {
    auto const bytes = std::as_bytes(std::span{&_header, std::size_t{1}});
    return {bytes.begin(), bytes.end()};
  }
} // namespace ao::library
