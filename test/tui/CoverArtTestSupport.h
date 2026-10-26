// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstddef>
#include <vector>

namespace ao::tui::test::support
{
  std::vector<std::byte> onePixelRedPng();

  /**
   * @brief The same decodable image, made a distinct resource by @p seed.
   *
   * Cover delivery keys on the resource id, so a burst of selection changes
   * needs bytes that hash differently while still decoding to a preview.
   */
  std::vector<std::byte> distinctPng(std::size_t seed);
} // namespace ao::tui::test::support
