// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/FileManifestBuilder.h>

#include "FileManifestValidation.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/FileTimestamp.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestView.h>
#include <ao/library/LibraryUri.h>
#include <ao/library/TrackLayout.h>
#include <ao/utility/ByteView.h>
#include <ao/utility/Hash128.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
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
    builder._segmentTrackIds.assign(view.segmentTrackIds().begin(), view.segmentTrackIds().end());
    builder._totalFrames = view.totalFrames();
    builder._header.segmentCount = view.segmentCount();
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

  FileManifestBuilder& FileManifestBuilder::mtime(std::optional<FileTimestamp> optVal)
  {
    _header.mtime(optVal);
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

  std::span<std::byte const> FileManifestBuilder::Prepared::bytes() const noexcept
  {
    return _carrierBytes.empty() ? std::as_bytes(std::span{&_header, std::size_t{1}})
                                 : std::span<std::byte const>{_carrierBytes};
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

  FileManifestBuilder::Prepared FileManifestBuilder::Unbound::bindCarrier(std::span<TrackId const> ids,
                                                                          std::uint64_t totalFrames) &&
  {
    AO_EXPECTS(!ids.empty() && ids.size() <= kMaxTrackSegmentOrdinal && totalFrames != 0);
    _header.trackId = kInvalidTrackId;
    _header.segmentCount = static_cast<std::uint16_t>(ids.size());
    auto prepared = Prepared{std::move(_uri), _header};
    prepared._carrierBytes.resize(sizeof(FileManifestHeader) + 8 + ids.size_bytes());
    std::memcpy(prepared._carrierBytes.data(), &_header, sizeof(_header));
    std::memcpy(prepared._carrierBytes.data() + sizeof(FileManifestHeader), &totalFrames, sizeof(totalFrames));
    std::memcpy(prepared._carrierBytes.data() + sizeof(FileManifestHeader) + 8, ids.data(), ids.size_bytes());
    AO_ENSURES(validateFileManifestPayload(prepared.bytes()));
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
    unboundHeader.segmentCount = 0;
    return Unbound{std::move(uri), unboundHeader};
  }

  std::vector<std::byte> FileManifestBuilder::serialize() const
  {
    auto const bytes = std::as_bytes(std::span{&_header, std::size_t{1}});
    auto encoded = std::vector<std::byte>{bytes.begin(), bytes.end()};

    if (!_segmentTrackIds.empty())
    {
      auto const extentBytes = utility::bytes::view(_totalFrames);
      auto const idBytes = std::as_bytes(std::span<TrackId const>{_segmentTrackIds});
      encoded.insert(encoded.end(), extentBytes.begin(), extentBytes.end());
      encoded.insert(encoded.end(), idBytes.begin(), idBytes.end());
    }

    return encoded;
  }
} // namespace ao::library
