// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

namespace ao::gtk::layout
{
  // Spacing presets
  constexpr int kSpacingSmall = 4;
  constexpr int kSpacingMedium = 6;
  constexpr int kSpacingLarge = 8;
  constexpr int kSpacingXLarge = 12;

  // Margin presets
  constexpr int kMarginSmall = 4;
  constexpr int kMarginMedium = 6;
  constexpr int kMarginLarge = 8;
  constexpr int kMarginXLarge = 12;

  // Icon size presets
  constexpr int kIconSizeSmall = 16;
  constexpr int kIconSizeXSmall = 12;

  // Transition/animation durations (milliseconds)
  constexpr int kTransitionDurationMs = 250;

  // UI Element sizing
  constexpr int kDefaultSidebarWidth = 330;
  constexpr int kMinCoverArtHeight = 50;
} // namespace ao::gtk::layout
