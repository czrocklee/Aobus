// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/utility/Uuid.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace ao::utility
{
  namespace
  {
    constexpr std::size_t kHexCharsPerByte = 2;
    constexpr std::size_t kUuidTimeLowByteCount = 4;
    constexpr std::size_t kUuidTimeMidByteCount = 2;
    constexpr std::size_t kUuidTimeHighByteCount = 2;
    constexpr std::size_t kUuidClockSeqByteCount = 2;
    constexpr std::size_t kUuidNodeByteCount = 6;
    constexpr auto kUuidGroupByteCounts = std::to_array<std::size_t>({kUuidTimeLowByteCount,
                                                                      kUuidTimeMidByteCount,
                                                                      kUuidTimeHighByteCount,
                                                                      kUuidClockSeqByteCount,
                                                                      kUuidNodeByteCount});
    constexpr auto kUuidHyphenCount = kUuidGroupByteCounts.size() - 1;
    constexpr auto kUuidTextLength = (kUuidByteCount * kHexCharsPerByte) + kUuidHyphenCount;
  } // namespace

  std::string formatUuid(UuidBytes const& id)
  {
    auto result = std::string{};
    result.reserve(kUuidTextLength);
    std::size_t byteIndex = 0;

    for (std::size_t groupIndex = 0; groupIndex < kUuidGroupByteCounts.size(); ++groupIndex)
    {
      if (groupIndex > 0)
      {
        result.push_back('-');
      }

      for (std::size_t groupByte = 0; groupByte < kUuidGroupByteCounts.at(groupIndex); ++groupByte)
      {
        result += std::format("{:02x}", static_cast<unsigned char>(id.at(byteIndex)));
        ++byteIndex;
      }
    }

    return result;
  }

  std::optional<UuidBytes> parseUuid(std::string_view const text)
  {
    if (text.size() != kUuidTextLength)
    {
      return std::nullopt;
    }

    auto result = UuidBytes{};
    std::size_t byteIndex = 0;
    std::size_t charIndex = 0;

    for (std::size_t groupIndex = 0; groupIndex < kUuidGroupByteCounts.size(); ++groupIndex)
    {
      if (groupIndex > 0)
      {
        if (text[charIndex] != '-')
        {
          return std::nullopt;
        }

        ++charIndex;
      }

      for (std::size_t groupByte = 0; groupByte < kUuidGroupByteCounts.at(groupIndex); ++groupByte)
      {
        auto const digits = text.substr(charIndex, kHexCharsPerByte);
        std::uint8_t value = 0;
        auto const [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value, 16);

        // For an unsigned type from_chars accepts no sign and no 0x prefix, so
        // the pair is consumed only when both characters are hexadecimal digits.
        if (error != std::errc{} || end != digits.data() + digits.size())
        {
          return std::nullopt;
        }

        result.at(byteIndex) = std::byte{value};
        ++byteIndex;
        charIndex += kHexCharsPerByte;
      }
    }

    return result;
  }
} // namespace ao::utility
