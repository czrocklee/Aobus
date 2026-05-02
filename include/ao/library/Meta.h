// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ao::library
{
  constexpr std::uint32_t kLibraryMetaMagic = 0x42534C52U;
  constexpr std::uint32_t kLibraryVersion = 1;

  enum class MetaRecordId : std::uint32_t
  {
    Header = 1,
  };

  struct MetaHeader final
  {
    std::uint32_t magic;
    std::uint32_t libraryVersion;
    std::uint32_t flags;
    std::uint64_t createdAtUnixMs;
    std::uint64_t migratedAtUnixMs;
    std::array<std::byte, 16> libraryId;
  };

  static_assert(sizeof(MetaHeader) == 48, "MetaHeader must be exactly 48 bytes");
  static_assert(std::is_trivially_copyable_v<MetaHeader>, "MetaHeader must be trivially copyable");
  static_assert(std::is_standard_layout_v<MetaHeader>, "MetaHeader must have standard layout");
}
