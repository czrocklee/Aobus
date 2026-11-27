// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "Decoder.h"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

namespace ao::media::file
{
  std::optional<std::uint16_t> decodeUint16(std::string_view text)
  {
    std::uint16_t result = 0;
    auto const* const first = text.data();
    auto const* const last = first + text.size();
    auto const [end, ec] = std::from_chars(first, last, result);
    return ec == std::errc{} && end == last ? std::optional<std::uint16_t>{result} : std::nullopt;
  }

  std::optional<std::uint16_t> decodeYear(std::string_view text)
  {
    // Any other leading number, such as the day of 21/01/2025, is not a year.
    constexpr std::size_t kYearDigits = 4;

    if (text.size() < kYearDigits || (text.size() > kYearDigits && text[kYearDigits] != '-'))
    {
      return std::nullopt;
    }

    return decodeUint16(text.substr(0, kYearDigits));
  }

  std::uint32_t bitrateFromBytes(std::uint64_t byteCount, std::chrono::milliseconds duration) noexcept
  {
    constexpr std::uint64_t kBitsPerByte = 8;
    constexpr std::uint64_t kMsPerSecond = 1000;

    if (duration <= std::chrono::milliseconds{0})
    {
      return 0;
    }

    return static_cast<std::uint32_t>((byteCount * kBitsPerByte * kMsPerSecond) /
                                      static_cast<std::uint64_t>(duration.count()));
  }

  NumberPair parseSlashPair(std::string_view text)
  {
    auto const slash = text.find('/');
    auto pair = NumberPair{};
    pair.optPrimary = decodeUint16(text.substr(0, slash));

    if (slash != std::string_view::npos)
    {
      pair.optSecondary = decodeUint16(text.substr(slash + 1));
    }

    return pair;
  }
} // namespace ao::media::file
