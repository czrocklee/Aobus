// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

namespace ao::test::ogg
{
  inline constexpr std::int64_t kNoGranulePosition = -1;

  struct Page final
  {
    std::uint8_t headerType = 0;
    std::int64_t granulePosition = kNoGranulePosition;
    std::uint32_t serialNumber = 1;
    std::uint32_t pageSequence = 0;
    std::uint8_t version = 0;
    std::vector<std::uint8_t> lacingValues{};
    std::vector<std::uint8_t> payload{};
  };

  /**
   * Lacing table terminating each of the given packet sizes on this page.
   */
  std::vector<std::uint8_t> lacingFor(std::initializer_list<std::size_t> packetSizes);

  /**
   * Payload bytes for the given packet sizes, each packet filled with its own
   * marker so reassembled boundaries stay identifiable.
   */
  std::vector<std::uint8_t> payloadFor(std::initializer_list<std::size_t> packetSizes, std::uint8_t firstMarker = 1);

  std::vector<std::uint8_t> makePage(Page const& page);

  std::vector<std::byte> makeStream(std::span<Page const> pages);

  std::vector<std::byte> toBytes(std::span<std::uint8_t const> bytes);
} // namespace ao::test::ogg
