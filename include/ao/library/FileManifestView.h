// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ao/Type.h"
#include "ao/library/FileManifestLayout.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace ao::library
{
  /**
   * FileManifestView - Unified view of a file manifest entry.
   */
  class FileManifestView final
  {
  public:
    explicit FileManifestView(std::span<std::byte const> data);

    TrackId trackId() const noexcept;
    std::uint64_t fileSize() const noexcept;
    std::uint64_t mtime() const noexcept;
    FileStatus status() const noexcept;

  private:
    FileManifestHeader const& header() const noexcept;

    std::span<std::byte const> _data;
  };
} // namespace ao::library
