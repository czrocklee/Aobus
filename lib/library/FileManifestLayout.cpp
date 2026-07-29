// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/FileManifestLayout.h>

#include <cstdint>

namespace ao::library
{
  void FileManifestHeader::fileSize(std::uint64_t val) noexcept
  {
    fileSizeLo = static_cast<std::uint32_t>(val);
    fileSizeHi = static_cast<std::uint32_t>(val >> 32);
  }

  void FileManifestHeader::mtime(std::uint64_t val) noexcept
  {
    mtimeLo = static_cast<std::uint32_t>(val);
    mtimeHi = static_cast<std::uint32_t>(val >> 32);
  }

  void FileManifestHeader::audioPayloadLength(std::uint64_t val) noexcept
  {
    audioPayloadLengthLo = static_cast<std::uint32_t>(val);
    audioPayloadLengthHi = static_cast<std::uint32_t>(val >> 32);
  }
} // namespace ao::library
