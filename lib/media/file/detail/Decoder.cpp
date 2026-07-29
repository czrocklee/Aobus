// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "Decoder.h"

#include <ao/utility/ByteView.h>

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace ao::media::file
{
  std::string decodeString(std::span<std::byte const> buf)
  {
    return std::string{utility::bytes::stringView(buf)};
  }

  std::optional<std::uint16_t> decodeUint16(std::string_view text)
  {
    std::uint16_t result = 0;
    auto const* data = text.data();
    auto [_, ec] = std::from_chars(data, data + text.size(), result);
    return ec == std::errc() ? std::optional{result} : std::nullopt;
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
