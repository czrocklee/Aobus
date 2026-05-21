// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstddef>
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
   * Returns an empty vector on invalid input.
   */
  std::vector<std::byte> base64Decode(std::string_view base64);
} // namespace ao::utility
