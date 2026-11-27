// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ao::media::file
{
  /**
   * @brief Decodes a complete unsigned decimal field. Empty, signed, trailing, or
   * out-of-range text yields nullopt.
   */
  std::optional<std::uint16_t> decodeUint16(std::string_view text);

  /**
   * @brief Decodes the year of a date field: four decimal digits that end the
   * text or are followed by '-', as in "2024" or "2024-05-17".
   */
  std::optional<std::uint16_t> decodeYear(std::string_view text);

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
   * by the Vorbis, ID3v2, and MP4 text-tag number handlers.
   */
  NumberPair parseSlashPair(std::string_view text);
} // namespace ao::media::file
