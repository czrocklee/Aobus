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
   * Returns std::nullopt on invalid input (an invalid character or malformed trailing bits);
   * a successfully decoded empty input yields an engaged optional holding an empty vector.
   */
  std::optional<std::vector<std::byte>> base64Decode(std::string_view base64);
} // namespace ao::utility
