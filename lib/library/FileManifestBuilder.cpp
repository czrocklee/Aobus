// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestView.h>
#include <ao/utility/Hash128.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ao::library
{
  FileManifestBuilder FileManifestBuilder::createNew()
  {
    return FileManifestBuilder{};
  }

  FileManifestBuilder FileManifestBuilder::fromView(FileManifestView const& view)
  {
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

  std::vector<std::byte> FileManifestBuilder::serialize() const
  {
    auto buffer = std::vector<std::byte>(sizeof(FileManifestHeader));
    std::memcpy(buffer.data(), &_header, sizeof(FileManifestHeader));
    return buffer;
  }
} // namespace ao::library
