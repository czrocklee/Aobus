// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ao/Type.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ao::library
{
  /**
   * FileStatus - Physical availability of a track.
   */
  enum class FileStatus : std::uint8_t
  {
    Available = 0,
    Missing = 1,
    Error = 2
  };

  /**
   * FileManifestHeader - POD struct for physical file tracking.
   * Total size: 24 bytes with 4-byte alignment.
   */
  struct FileManifestHeader final
  {
    static constexpr std::size_t kPaddingSize = 3;

    // 4-byte section
    TrackId trackId{};          // 4B: Links to the logical track
    std::uint32_t fileSizeLo{}; // 4B: Lower 32 bits of file size
    std::uint32_t fileSizeHi{}; // 4B: Upper 32 bits of file size
    std::uint32_t mtimeLo{};    // 4B: Lower 32 bits of mtime
    std::uint32_t mtimeHi{};    // 4B: Upper 32 bits of mtime

    // 1-byte section
    FileStatus status = FileStatus::Available; // 1B

    // 3 bytes padding to reach 24 bytes total
    std::array<std::byte, kPaddingSize> padding{};

    // Reconstruct 64-bit values
    std::uint64_t fileSize() const noexcept { return (static_cast<std::uint64_t>(fileSizeHi) << 32) | fileSizeLo; }

    std::uint64_t mtime() const noexcept { return (static_cast<std::uint64_t>(mtimeHi) << 32) | mtimeLo; }

    // Set 64-bit values
    void fileSize(std::uint64_t val) noexcept
    {
      fileSizeLo = static_cast<std::uint32_t>(val);
      fileSizeHi = static_cast<std::uint32_t>(val >> 32);
    }

    void mtime(std::uint64_t val) noexcept
    {
      mtimeLo = static_cast<std::uint32_t>(val);
      mtimeHi = static_cast<std::uint32_t>(val >> 32);
    }
  };

  constexpr std::size_t kFileManifestHeaderSize = 24;
  constexpr std::size_t kFileManifestHeaderAlignment = 4;

  static_assert(sizeof(FileManifestHeader) == kFileManifestHeaderSize, "FileManifestHeader must be exactly 24 bytes");
  static_assert(alignof(FileManifestHeader) == kFileManifestHeaderAlignment,
                "FileManifestHeader must have 4-byte alignment");
} // namespace ao::library
