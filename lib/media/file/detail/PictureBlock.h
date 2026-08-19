// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/PictureType.h>

#include <cstddef>
#include <optional>
#include <span>

namespace ao::media::file::detail
{
  struct PictureBlock final
  {
    PictureType type = PictureType::Other;
    std::span<std::byte const> bytes;
  };

  /**
   * @brief Parses a METADATA_BLOCK_PICTURE body: a big-endian picture type,
   * length-prefixed MIME and description strings, four image scalars, and the
   * length-prefixed image bytes.
   *
   * FLAC carries this body in a PICTURE metadata block while Vorbis and Opus
   * carry it Base64-encoded inside a comment, so both reach the same layout.
   * A type outside the ID3v2 role range normalizes to Other.
   *
   * @return The role and borrowed image bytes, or nullopt when the structure
   *         does not exactly fill its payload.
   */
  std::optional<PictureBlock> parsePictureBlock(std::span<std::byte const> payload) noexcept;
} // namespace ao::media::file::detail
