// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace rs::core
{
  constexpr std::uint32_t kLibraryMetaMagic = 0x42534C52U;
  constexpr std::uint16_t kLibraryMetaHeaderVersion = 1;
  constexpr std::uint16_t kLibrarySchemaVersion = 1;

  enum class MetaRecordId : std::uint32_t
  {
    Header = 1,
  };

  struct LibraryMetaHeader final
  {
    std::uint32_t magic;
    std::uint16_t headerVersion;
    std::uint16_t librarySchemaVersion;
    std::uint16_t trackLayoutVersion;
    std::uint16_t listLayoutVersion;
    std::uint32_t flags;
    std::uint64_t createdAtUnixMs;
    std::uint64_t migratedAtUnixMs;
    std::array<std::byte, 16> libraryId;
  };

  static_assert(sizeof(LibraryMetaHeader) == 48, "LibraryMetaHeader must be exactly 48 bytes");
  static_assert(std::is_trivially_copyable_v<LibraryMetaHeader>, "LibraryMetaHeader must be trivially copyable");
  static_assert(std::is_standard_layout_v<LibraryMetaHeader>, "LibraryMetaHeader must have standard layout");
}
