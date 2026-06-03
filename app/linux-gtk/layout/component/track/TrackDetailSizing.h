// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>

namespace ao::gtk::layout
{
  enum class LayoutMode : std::uint8_t
  {
    Standard,
    Wide
  };

  LayoutMode computeLayoutMode(std::int32_t width);
  std::int32_t coverArtSideForWidth(std::int32_t width, std::int32_t targetSize);

  constexpr std::int32_t kDefaultCoverArtTargetSize = 250;
  constexpr std::int32_t kStandardWidthThreshold = 550;
} // namespace ao::gtk::layout
