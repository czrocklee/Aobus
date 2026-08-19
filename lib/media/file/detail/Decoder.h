// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <boost/endian/detail/order.hpp>

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

  /**
   * @brief Reads one 32-bit field at @p offset and advances it. Returns nullopt
   * when the field does not fit. Shared by the length-prefixed structures of
   * Vorbis comment lists and METADATA_BLOCK_PICTURE bodies, which use opposite
   * byte orders.
   */
  std::optional<std::uint32_t> readU32(std::span<std::byte const> bytes,
                                       std::size_t& offset,
                                       boost::endian::order order) noexcept;

  /**
   * @brief Reads a 32-bit length followed by that many bytes at @p offset and
   * advances it. Returns nullopt when either part does not fit.
   */
  std::optional<std::span<std::byte const>> readSized(std::span<std::byte const> bytes,
                                                      std::size_t& offset,
                                                      boost::endian::order order) noexcept;

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
