// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/FileTimestamp.h>
#include <ao/utility/Hash128.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

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
   * Total size: 52 bytes with 4-byte alignment.
   */
  struct FileManifestHeader final
  {
    // 4-byte section
    TrackId trackId{};                // 4B: Links to the logical track
    std::uint32_t fileSizeLo{};       // 4B: Lower 32 bits of file size
    std::uint32_t fileSizeHi{};       // 4B: Upper 32 bits of file size
    std::uint32_t mtimeSecondsLo{};   // 4B: Lower 32 bits of the mtime Unix seconds
    std::uint32_t mtimeSecondsHi{};   // 4B: Upper 32 bits of the mtime Unix seconds
    std::uint32_t mtimeNanoseconds{}; // 4B: Sub-second fraction, in [0, 1000000000)

    std::uint32_t audioPayloadLengthLo{}; // 4B: Lower 32 bits of audio payload length
    std::uint32_t audioPayloadLengthHi{}; // 4B: Upper 32 bits of audio payload length

    std::array<std::byte, 16> audioSignatureBytes{}; // 16B: 128-bit audio payload signature

    // 1-byte section
    FileStatus status = FileStatus::Available; // 1B
    std::uint8_t hasMtime = 0;                 // 1B: 1 marks the three time words as a present instant

    std::uint16_t segmentCount{};

    // Reconstruct stored values
    std::uint64_t fileSize() const noexcept { return (static_cast<std::uint64_t>(fileSizeHi) << 32) | fileSizeLo; }

    // The stored instant when hasMtime is 1, absent when the flag is 0. Absence
    // canonicalizes all three time words to zero, so epoch zero stays present.
    std::optional<FileTimestamp> mtime() const noexcept;

    std::uint64_t audioPayloadLength() const noexcept
    {
      return (static_cast<std::uint64_t>(audioPayloadLengthHi) << 32) | audioPayloadLengthLo;
    }

    utility::Hash128 audioSignature() const noexcept { return {.bytes = audioSignatureBytes}; }

    // Set stored values
    void fileSize(std::uint64_t val) noexcept;

    // Stores the instant, or canonicalizes the three time words for absence.
    void mtime(std::optional<FileTimestamp> optVal) noexcept;

    void audioPayloadLength(std::uint64_t val) noexcept;

    void audioSignature(utility::Hash128 val) noexcept { audioSignatureBytes = val.bytes; }
  };

  constexpr std::size_t kFileManifestHeaderSize = 52;
  constexpr std::size_t kFileManifestHeaderAlignment = 4;

  static_assert(sizeof(FileManifestHeader) == kFileManifestHeaderSize, "FileManifestHeader must be exactly 52 bytes");
  static_assert(alignof(FileManifestHeader) == kFileManifestHeaderAlignment,
                "FileManifestHeader must have 4-byte alignment");
  static_assert(std::has_unique_object_representations_v<FileManifestHeader>,
                "FileManifestHeader must have no padding");
  static_assert(offsetof(FileManifestHeader, trackId) == 0);
  static_assert(offsetof(FileManifestHeader, fileSizeLo) ==
                offsetof(FileManifestHeader, trackId) + sizeof(FileManifestHeader::trackId));
  static_assert(offsetof(FileManifestHeader, fileSizeHi) ==
                offsetof(FileManifestHeader, fileSizeLo) + sizeof(FileManifestHeader::fileSizeLo));
  static_assert(offsetof(FileManifestHeader, mtimeSecondsLo) ==
                offsetof(FileManifestHeader, fileSizeHi) + sizeof(FileManifestHeader::fileSizeHi));
  static_assert(offsetof(FileManifestHeader, mtimeSecondsHi) ==
                offsetof(FileManifestHeader, mtimeSecondsLo) + sizeof(FileManifestHeader::mtimeSecondsLo));
  static_assert(offsetof(FileManifestHeader, mtimeNanoseconds) ==
                offsetof(FileManifestHeader, mtimeSecondsHi) + sizeof(FileManifestHeader::mtimeSecondsHi));
  static_assert(offsetof(FileManifestHeader, audioPayloadLengthLo) ==
                offsetof(FileManifestHeader, mtimeNanoseconds) + sizeof(FileManifestHeader::mtimeNanoseconds));
  static_assert(offsetof(FileManifestHeader, audioPayloadLengthHi) ==
                offsetof(FileManifestHeader, audioPayloadLengthLo) + sizeof(FileManifestHeader::audioPayloadLengthLo));
  static_assert(offsetof(FileManifestHeader, audioSignatureBytes) ==
                offsetof(FileManifestHeader, audioPayloadLengthHi) + sizeof(FileManifestHeader::audioPayloadLengthHi));
  static_assert(offsetof(FileManifestHeader, status) ==
                offsetof(FileManifestHeader, audioSignatureBytes) + sizeof(FileManifestHeader::audioSignatureBytes));
  static_assert(offsetof(FileManifestHeader, hasMtime) ==
                offsetof(FileManifestHeader, status) + sizeof(FileManifestHeader::status));
  static_assert(offsetof(FileManifestHeader, segmentCount) ==
                offsetof(FileManifestHeader, hasMtime) + sizeof(FileManifestHeader::hasMtime));
} // namespace ao::library
