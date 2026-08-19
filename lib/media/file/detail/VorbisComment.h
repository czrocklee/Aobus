// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "Content.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ao::media::file::detail
{
  /**
   * @brief Whether bytes after the comment list are acceptable.
   *
   * A FLAC VORBIS_COMMENT block ends exactly at its list, while an Opus
   * OpusTags packet may carry padding or further data after it.
   */
  enum class VorbisCommentTrailing : std::uint8_t
  {
    Rejected,
    Allowed,
  };

  struct VorbisCommentField final
  {
    std::string_view key;
    std::string_view value;
  };

  /**
   * @brief Parses a Vorbis comment list: a length-prefixed vendor string, a
   * comment count, and that many length-prefixed comments, all little-endian.
   * @return The raw comments, or nullopt when the structure does not fit its
   *         payload or trailing bytes are present and rejected.
   */
  std::optional<std::vector<std::string_view>> parseVorbisComments(std::span<std::byte const> payload,
                                                                   VorbisCommentTrailing trailing);

  /**
   * @brief Splits one "KEY=value" comment.
   * @return Its two halves, or nullopt when the comment carries no separator.
   */
  std::optional<VorbisCommentField> splitVorbisComment(std::string_view comment);

  /**
   * @brief Applies one comment to the content field its key maps to.
   * @return False when the key has no mapping, leaving the caller free to
   *         interpret it.
   */
  bool applyVorbisComment(ContentBuilder& builder, VorbisCommentField field);
} // namespace ao::media::file::detail
