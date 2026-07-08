// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/utility/Hash128.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ao::library
{
  class FileManifestView;

  /**
   * FileManifestBuilder - Fluent builder for constructing file manifest binary data.
   */
  class FileManifestBuilder final
  {
  public:
    static FileManifestBuilder makeEmpty();
    static FileManifestBuilder fromView(FileManifestView const& view);

    FileManifestBuilder& trackId(TrackId val);
    FileManifestBuilder& fileSize(std::uint64_t val);
    FileManifestBuilder& mtime(std::uint64_t val);
    FileManifestBuilder& audioPayloadLength(std::uint64_t val);
    FileManifestBuilder& audioSignature(utility::Hash128 val);
    FileManifestBuilder& status(FileStatus val);

    std::vector<std::byte> serialize() const;

  private:
    FileManifestHeader _header;
  };
} // namespace ao::library
