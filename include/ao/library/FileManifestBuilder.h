// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ao/Type.h"
#include "ao/library/FileManifestLayout.h"
#include "ao/library/FileManifestView.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ao::library
{
  /**
   * FileManifestBuilder - Fluent builder for constructing file manifest binary data.
   */
  class FileManifestBuilder final
  {
  public:
    static FileManifestBuilder createNew();
    static FileManifestBuilder fromView(FileManifestView const& view);

    FileManifestBuilder& trackId(TrackId val);
    FileManifestBuilder& fileSize(std::uint64_t val);
    FileManifestBuilder& mtime(std::uint64_t val);
    FileManifestBuilder& status(FileStatus val);

    std::vector<std::byte> serialize() const;

  private:
    FileManifestHeader _header;
  };
} // namespace ao::library
