// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "ApeTag.h"

#include <ao/Error.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace ao::media::file::mpeg
{
  namespace
  {
    constexpr std::string_view kApeV2Magic = "APETAGEX";
    constexpr std::size_t kApeV2FooterSize = 32;
    constexpr std::size_t kApeV2SizeOffset = 12;
    constexpr std::size_t kApeV2FlagsOffset = 20;
    constexpr std::uint32_t kApeV2ContainsHeaderFlag = 0x80000000U;

    std::uint32_t readLittleEndianU32(std::byte const* data) noexcept
    {
      return std::to_integer<std::uint32_t>(data[0]) | (std::to_integer<std::uint32_t>(data[1]) << 8U) |
             (std::to_integer<std::uint32_t>(data[2]) << 16U) | (std::to_integer<std::uint32_t>(data[3]) << 24U);
    }
  } // namespace

  Result<std::size_t> readApeTagStart(std::span<std::byte const> bytes)
  {
    if (bytes.size() < kApeV2FooterSize)
    {
      return makeError(Error::Code::CorruptData, "missing ape trailing tag footer");
    }

    auto const* const footer = bytes.data() + bytes.size() - kApeV2FooterSize;

    if (std::memcmp(footer, kApeV2Magic.data(), kApeV2Magic.size()) != 0)
    {
      return makeError(Error::Code::CorruptData, "missing ape trailing tag footer");
    }

    auto const tagSize = static_cast<std::size_t>(readLittleEndianU32(footer + kApeV2SizeOffset));

    if (tagSize < kApeV2FooterSize || tagSize > bytes.size())
    {
      return makeError(Error::Code::CorruptData, "invalid apev2 trailing tag size");
    }

    auto totalTagSize = tagSize;
    auto const flags = readLittleEndianU32(footer + kApeV2FlagsOffset);

    if ((flags & kApeV2ContainsHeaderFlag) != 0)
    {
      if (tagSize > bytes.size() - kApeV2FooterSize)
      {
        return makeError(Error::Code::CorruptData, "invalid apev2 trailing tag header size");
      }

      totalTagSize += kApeV2FooterSize;
      auto const* const header = bytes.data() + bytes.size() - totalTagSize;

      if (std::memcmp(header, kApeV2Magic.data(), kApeV2Magic.size()) != 0)
      {
        return makeError(Error::Code::CorruptData, "invalid apev2 trailing tag header");
      }
    }

    return bytes.size() - totalTagSize;
  }
} // namespace ao::media::file::mpeg
