// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

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
  inline std::string decodeString(std::span<std::byte const> buf)
  {
    return std::string{utility::bytes::stringView(buf)};
  }

  inline std::optional<std::uint16_t> decodeUint16(std::string_view text)
  {
    std::uint16_t result = 0;
    auto const* data = text.data();
    auto [_, ec] = std::from_chars(data, data + text.size(), result);
    return ec == std::errc() ? std::optional{result} : std::nullopt;
  }

  /**
   * @brief Average bitrate (bits/sec) implied by spreading @p byteCount over
   * @p duration. Returns 0 for a non-positive duration. Shared by the FLAC, MP4,
   * and MPEG property loaders, which otherwise re-derive the same formula.
   */
  inline std::uint32_t bitrateFromBytes(std::uint64_t byteCount, std::chrono::milliseconds duration) noexcept
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

  /**
   * @brief Parsed "primary/secondary" numeric tag value (e.g. track "3/12").
   * Either component is nullopt when absent or non-numeric.
   */
  struct NumberPair final
  {
    std::optional<std::uint16_t> optPrimary;
    std::optional<std::uint16_t> optSecondary;
  };

  /**
   * @brief Splits a slash-delimited numeric field into its two components. With
   * no '/', the whole string is the primary and the secondary is nullopt. Shared
   * by the FLAC Vorbis-comment and ID3v2 text-frame number handlers.
   */
  inline NumberPair parseSlashPair(std::string_view text)
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
