// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

namespace ao::gtk::layout
{
  // Spacing presets (Used for Gtk::Box spacing and other UI gaps in C++)
  constexpr int kSpacingXSmall = 2;
  constexpr int kSpacingSmall = 4;
  constexpr int kSpacingMedium = 6;
  constexpr int kSpacingLarge = 8;
  constexpr int kSpacingXLarge = 12;

  // Icon size presets
  constexpr int kIconSizeSmall = 16;
  constexpr int kIconSizeXSmall = 12;

  // UI Element sizing
  constexpr int kDefaultListPaneWidth = 330;
  constexpr int kMinCoverArtHeight = 50;
} // namespace ao::gtk::layout
