// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ao::library
{
  // Every persisted record is written in native byte order and word size, so the
  // format is defined only for little-endian 64-bit hosts.
  static_assert(std::endian::native == std::endian::little, "The library format requires a little-endian host");
  static_assert(sizeof(void*) == 8, "The library format requires a 64-bit host");

  constexpr std::uint32_t kMetadataMagic = 0x42534C52U;
  constexpr std::uint32_t kLibraryVersion = 10;
  constexpr std::size_t kMetadataHeaderSize = 40;
  constexpr std::size_t kIdSpaceSize = 8;
  constexpr std::uint32_t kMetadataHeaderRecordId = 1;
  constexpr std::uint32_t kLibraryRevisionRecordId = 2;
  constexpr std::uint32_t kIdSpaceRecordId = 3;

  struct MetadataHeader final
  {
    std::uint32_t magic{};
    std::uint32_t libraryVersion{};
    std::uint32_t flags{};
    // Must be zero. It names the bytes alignment would otherwise leave as
    // indeterminate padding before createdTime.
    std::uint32_t reserved{};
    std::chrono::sys_time<std::chrono::milliseconds> createdTime{};
    // Random and fixed when the database is created. It names this database and
    // its id history, so no restore carries it into another database.
    std::array<std::byte, 16> libraryId{};
  };

  static_assert(sizeof(MetadataHeader) == kMetadataHeaderSize, "MetadataHeader must be exactly 40 bytes");
  static_assert(std::is_trivially_copyable_v<MetadataHeader>, "MetadataHeader must be trivially copyable");
  static_assert(std::is_standard_layout_v<MetadataHeader>, "MetadataHeader must have standard layout");
  static_assert(std::has_unique_object_representations_v<MetadataHeader>, "MetadataHeader must have no padding");
  static_assert(sizeof(std::chrono::sys_time<std::chrono::milliseconds>) == 8);
  static_assert(offsetof(MetadataHeader, magic) == 0);
  static_assert(offsetof(MetadataHeader, libraryVersion) ==
                offsetof(MetadataHeader, magic) + sizeof(MetadataHeader::magic));
  static_assert(offsetof(MetadataHeader, flags) ==
                offsetof(MetadataHeader, libraryVersion) + sizeof(MetadataHeader::libraryVersion));
  static_assert(offsetof(MetadataHeader, reserved) == offsetof(MetadataHeader, flags) + sizeof(MetadataHeader::flags));
  static_assert(offsetof(MetadataHeader, createdTime) ==
                offsetof(MetadataHeader, reserved) + sizeof(MetadataHeader::reserved));
  static_assert(offsetof(MetadataHeader, libraryId) ==
                offsetof(MetadataHeader, createdTime) + sizeof(MetadataHeader::createdTime));

  /** The nil id names no library, so creation never draws it and open rejects it. */
  constexpr bool isNilLibraryId(std::array<std::byte, 16> const& libraryId) noexcept
  {
    return libraryId == std::array<std::byte, 16>{};
  }

  /**
   * Allocation history of the Track and List id spaces.
   *
   * The last ids are the highest ever committed, so a deleted id is never
   * allocated again, even after its table is cleared.
   */
  struct IdSpace final
  {
    std::uint32_t lastTrackId{};
    std::uint32_t lastListId{};

    bool operator==(IdSpace const&) const = default;
  };

  static_assert(sizeof(IdSpace) == kIdSpaceSize, "IdSpace must be exactly 8 bytes");
  static_assert(std::is_trivially_copyable_v<IdSpace>, "IdSpace must be trivially copyable");
  static_assert(std::has_unique_object_representations_v<IdSpace>, "IdSpace must have no padding");
  static_assert(offsetof(IdSpace, lastTrackId) == 0);
  static_assert(offsetof(IdSpace, lastListId) == offsetof(IdSpace, lastTrackId) + sizeof(IdSpace::lastTrackId));
} // namespace ao::library
