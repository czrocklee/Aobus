// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/FileManifestLayout.h>

#include <ao/FileTimestamp.h>

#include <cstdint>
#include <optional>

namespace ao::library
{
  std::optional<FileTimestamp> FileManifestHeader::mtime() const noexcept
  {
    if (hasMtime == 0U)
    {
      return std::nullopt;
    }

    auto const secondsBits = (static_cast<std::uint64_t>(mtimeSecondsHi) << 32) | mtimeSecondsLo;
    return FileTimestamp{.seconds = static_cast<std::int64_t>(secondsBits), .nanoseconds = mtimeNanoseconds};
  }

  void FileManifestHeader::fileSize(std::uint64_t val) noexcept
  {
    fileSizeLo = static_cast<std::uint32_t>(val);
    fileSizeHi = static_cast<std::uint32_t>(val >> 32);
  }

  void FileManifestHeader::mtime(std::optional<FileTimestamp> optVal) noexcept
  {
    if (!optVal)
    {
      hasMtime = 0U;
      mtimeSecondsLo = 0U;
      mtimeSecondsHi = 0U;
      mtimeNanoseconds = 0U;
      return;
    }

    auto const secondsBits = static_cast<std::uint64_t>(optVal->seconds);
    hasMtime = 1U;
    mtimeSecondsLo = static_cast<std::uint32_t>(secondsBits);
    mtimeSecondsHi = static_cast<std::uint32_t>(secondsBits >> 32);
    mtimeNanoseconds = optVal->nanoseconds;
  }

  void FileManifestHeader::audioPayloadLength(std::uint64_t val) noexcept
  {
    audioPayloadLengthLo = static_cast<std::uint32_t>(val);
    audioPayloadLengthHi = static_cast<std::uint32_t>(val >> 32);
  }
} // namespace ao::library
