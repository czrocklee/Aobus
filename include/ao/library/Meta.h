// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ao::library
{
  constexpr std::uint32_t kLibraryMetaMagic = 0x42534C52U;
  constexpr std::uint32_t kLibraryVersion = 2;
  constexpr std::size_t kMetaHeaderByteCount = 40;

  enum class MetaRecordId : std::uint8_t
  {
    Header = 1,
  };

  struct MetaHeader final
  {
    std::uint32_t magic{};
    std::uint32_t libraryVersion{};
    std::uint32_t flags{};
    std::chrono::sys_time<std::chrono::milliseconds> createdTime{};
    std::array<std::byte, 16> libraryId{};
  };

  static_assert(sizeof(MetaHeader) == kMetaHeaderByteCount, "MetaHeader must be exactly 40 bytes");
  static_assert(std::is_trivially_copyable_v<MetaHeader>, "MetaHeader must be trivially copyable");
  static_assert(std::is_standard_layout_v<MetaHeader>, "MetaHeader must have standard layout");
}
