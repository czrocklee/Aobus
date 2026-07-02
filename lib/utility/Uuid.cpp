// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/utility/Uuid.h>

#include <array>
#include <cstddef>
#include <format>
#include <string>

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
} // namespace ao::utility
