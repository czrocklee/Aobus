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
  constexpr std::uint32_t kMetadataMagic = 0x42534C52U;
  constexpr std::uint32_t kLibraryVersion = 4;
  constexpr std::size_t kMetadataHeaderSize = 40;
  constexpr std::uint32_t kMetadataHeaderRecordId = 1;
  constexpr std::uint32_t kLibraryRevisionRecordId = 2;

  struct MetadataHeader final
  {
    std::uint32_t magic{};
    std::uint32_t libraryVersion{};
    std::uint32_t flags{};
    std::chrono::sys_time<std::chrono::milliseconds> createdTime{};
    std::array<std::byte, 16> libraryId{};
  };

  static_assert(sizeof(MetadataHeader) == kMetadataHeaderSize, "MetadataHeader must be exactly 40 bytes");
  static_assert(std::is_trivially_copyable_v<MetadataHeader>, "MetadataHeader must be trivially copyable");
  static_assert(std::is_standard_layout_v<MetadataHeader>, "MetadataHeader must have standard layout");
} // namespace ao::library
