// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>

namespace ao::gtk::layout
{
  std::int32_t coverArtSideForWidth(std::int32_t width, std::int32_t targetSize);

  constexpr std::int32_t kDefaultCoverArtTargetSize = 250;
} // namespace ao::gtk::layout
