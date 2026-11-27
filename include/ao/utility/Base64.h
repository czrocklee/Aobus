// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::utility
{
  /**
   * Encodes binary data to a Base64 string.
   */
  std::string base64Encode(std::span<std::byte const> data);

  /**
   * Decodes a Base64 string to binary data.
   * ASCII whitespace is ignored. Padding is optional; if supplied, it must complete the final quartet.
   * Returns std::nullopt on invalid characters, invalid padding, or nonzero unused trailing bits;
   * a successfully decoded empty input yields an engaged optional holding an empty vector.
   */
  std::optional<std::vector<std::byte>> base64Decode(std::string_view base64);
} // namespace ao::utility
