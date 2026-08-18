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

  FileManifestBuilder::Unbound::Unbound(LibraryUri uri, FileManifestHeader header)
    : _uri{std::move(uri)}, _header{header}
  {
  }

  FileManifestBuilder::Prepared FileManifestBuilder::Unbound::bind(TrackId const id) && noexcept
  {
    AO_EXPECTS(_header.trackId == kInvalidTrackId, "Cannot bind a file manifest that was already bound");
    AO_EXPECTS(id != kInvalidTrackId, "Cannot bind a file manifest to Track zero");

    _header.trackId = id;
    auto prepared = Prepared{std::move(_uri), _header};

    AO_ENSURES(validateFileManifestPayload(prepared.bytes()),
               "Bound file manifest payload failed the complete record validator");
    return prepared;
  }

  Result<FileManifestBuilder::Unbound> FileManifestBuilder::validate(std::string_view const uri) const
  {
    auto uriRes = LibraryUri::parse(uri);

    if (!uriRes)
    {
      return std::unexpected{uriRes.error()};
    }

    return validate(std::move(*uriRes));
  }

  Result<FileManifestBuilder::Unbound> FileManifestBuilder::validate(LibraryUri uri) const
  {
    auto const key = detail::PaddedFileManifestKey{uri.value()};

    if (auto const keyRes = validateFileManifestKey(key.bytes()); !keyRes)
    {
      return std::unexpected{keyRes.error()};
    }

    if (auto const factsRes = validateManifestFacts(_header); !factsRes)
    {
      return std::unexpected{factsRes.error()};
    }

    auto unboundHeader = _header;
    unboundHeader.trackId = kInvalidTrackId;
    return Unbound{std::move(uri), unboundHeader};
  }

  std::vector<std::byte> FileManifestBuilder::serialize() const
  {
    auto const bytes = std::as_bytes(std::span{&_header, std::size_t{1}});
    return {bytes.begin(), bytes.end()};
  }
} // namespace ao::library
