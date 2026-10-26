// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/utility/ByteView.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace ao::media
{
  /**
   * @brief Whether bytes after the comment list are acceptable.
   *
   * A FLAC VORBIS_COMMENT block ends exactly at its list, while an OpusTags
   * packet may carry padding or further data after it.
   */
  enum class VorbisCommentTrailing : std::uint8_t
  {
    Rejected,
    Allowed,
  };

  /**
   * @brief Visits the raw entries of a little-endian Vorbis comment list.
   *
   * Skips the length-prefixed vendor, reads the count, then visits every
   * length-prefixed entry in order. Lengths and count are little-endian u32.
   * Entries are preserved, including empty and separatorless entries
   * and embedded NUL bytes. Traversal performs no allocation or normalization;
   * the callable's own construction and behavior remain its responsibility.
   * The visitor is passed by value and invoked as an lvalue for each entry,
   * synchronously on the calling thread. Views are borrowed from payload;
   * retaining one requires keeping its bytes alive and unchanged. Visitor
   * exceptions propagate to the caller.
   *
   * @return True only after the complete list and trailing-byte policy have
   * been validated. False means malformed input, rejected trailing bytes, or
   * a visitor returning false to stop immediately. A visitor may observe a
   * valid prefix before a later failure; callers needing atomic publication
   * must stage their effects until this function returns true.
   */
  template<typename Visitor>
  bool tryVisitVorbisComments(
    std::span<std::byte const> payload,
    VorbisCommentTrailing trailing,
    Visitor visitor) noexcept(std::is_nothrow_invocable_r_v<bool, Visitor&, std::string_view const&>)
  {
    auto const tryReadU32 = [&payload](std::uint32_t& value) noexcept
    {
      if (payload.size() < sizeof(std::uint32_t))
      {
        return false;
      }

      value = std::to_integer<std::uint32_t>(payload[0]) | (std::to_integer<std::uint32_t>(payload[1]) << 8) |
              (std::to_integer<std::uint32_t>(payload[2]) << 16) | (std::to_integer<std::uint32_t>(payload[3]) << 24);
      payload = payload.subspan(sizeof(std::uint32_t));
      return true;
    };

    std::uint32_t vendorLength = 0;

    if (!tryReadU32(vendorLength) || vendorLength > payload.size())
    {
      return false;
    }

    payload = payload.subspan(vendorLength);
    std::uint32_t count = 0;

    if (!tryReadU32(count))
    {
      return false;
    }

    for (std::uint32_t index = 0; index < count; ++index)
    {
      std::uint32_t length = 0;

      if (!tryReadU32(length) || length > payload.size())
      {
        return false;
      }

      auto const comment = utility::bytes::stringView(payload.first(length));

      if (!static_cast<bool>(visitor(comment)))
      {
        return false;
      }

      payload = payload.subspan(length);
    }

    return trailing == VorbisCommentTrailing::Allowed || payload.empty();
  }
} // namespace ao::media
