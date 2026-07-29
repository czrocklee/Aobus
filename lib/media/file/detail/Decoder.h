// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ao::media::file
{
  std::string decodeString(std::span<std::byte const> buf);

  std::optional<std::uint16_t> decodeUint16(std::string_view text);

  /**
   * @brief Average bitrate (bits/sec) implied by spreading @p byteCount over
   * @p duration. Returns 0 for a non-positive duration. Shared by the FLAC, MP4,
   * and MPEG property loaders, which otherwise re-derive the same formula.
   */
  std::uint32_t bitrateFromBytes(std::uint64_t byteCount, std::chrono::milliseconds duration) noexcept;

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
  NumberPair parseSlashPair(std::string_view text);
} // namespace ao::media::file
