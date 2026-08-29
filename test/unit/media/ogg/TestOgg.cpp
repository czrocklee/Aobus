// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/media/ogg/TestOgg.h"

#include "lib/media/ogg/PageLayout.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

namespace ao::test::ogg
{
  namespace
  {
    using media::ogg::kCapturePattern;
    using media::ogg::kContinuationLacingValue;

    void appendLe32(std::vector<std::uint8_t>& output, std::uint32_t const value)
    {
      for (std::size_t index = 0; index < sizeof(value); ++index)
      {
        output.push_back(static_cast<std::uint8_t>((value >> (index * 8U)) & 0xFFU));
      }
    }

    void appendLe64(std::vector<std::uint8_t>& output, std::int64_t const value)
    {
      auto const unsignedValue = static_cast<std::uint64_t>(value);

      for (std::size_t index = 0; index < sizeof(unsignedValue); ++index)
      {
        output.push_back(static_cast<std::uint8_t>((unsignedValue >> (index * 8U)) & 0xFFU));
      }
    }
  } // namespace

  std::vector<std::uint8_t> lacingFor(std::initializer_list<std::size_t> const packetSizes)
  {
    auto lacing = std::vector<std::uint8_t>{};

    for (auto size : packetSizes)
    {
      while (size >= kContinuationLacingValue)
      {
        lacing.push_back(kContinuationLacingValue);
        size -= kContinuationLacingValue;
      }

      lacing.push_back(static_cast<std::uint8_t>(size));
    }

    return lacing;
  }

  std::vector<std::uint8_t> payloadFor(std::initializer_list<std::size_t> const packetSizes,
                                       std::uint8_t const firstMarker)
  {
    auto payload = std::vector<std::uint8_t>{};
    auto marker = firstMarker;

    for (auto const size : packetSizes)
    {
      payload.insert(payload.end(), size, marker);
      ++marker;
    }

    return payload;
  }

  std::vector<std::uint8_t> makePage(Page const& page)
  {
    auto bytes = std::vector<std::uint8_t>{};

    for (char const character : kCapturePattern)
    {
      bytes.push_back(static_cast<std::uint8_t>(character));
    }

    bytes.push_back(page.version);
    bytes.push_back(page.headerType);
    appendLe64(bytes, page.granulePosition);
    appendLe32(bytes, page.serialNumber);
    appendLe32(bytes, page.pageSequence);
    appendLe32(bytes, 0); // Checksum; the demuxer documents that it is not verified.
    bytes.push_back(static_cast<std::uint8_t>(page.lacingValues.size()));
    bytes.insert(bytes.end(), page.lacingValues.begin(), page.lacingValues.end());
    bytes.insert(bytes.end(), page.payload.begin(), page.payload.end());
    return bytes;
  }

  std::vector<std::byte> makeStream(std::span<Page const> const pages)
  {
    auto bytes = std::vector<std::uint8_t>{};

    for (auto const& page : pages)
    {
      auto const pageBytes = makePage(page);
      bytes.insert(bytes.end(), pageBytes.begin(), pageBytes.end());
    }

    return toBytes(bytes);
  }

  std::vector<std::byte> toBytes(std::span<std::uint8_t const> const bytes)
  {
    auto result = std::vector<std::byte>{};
    result.reserve(bytes.size());

    for (auto const byte : bytes)
    {
      result.push_back(static_cast<std::byte>(byte));
    }

    return result;
  }
} // namespace ao::test::ogg
