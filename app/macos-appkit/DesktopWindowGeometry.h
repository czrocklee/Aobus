// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <CoreGraphics/CGBase.h>

namespace ao::appkit
{
  inline constexpr CGFloat kMinimumWindowWidth = 760;
  inline constexpr CGFloat kMinimumWindowHeight = 520;
  inline constexpr CGFloat kInitialWindowWidth = 1240;
  inline constexpr CGFloat kInitialWindowHeight = 800;
  // Compact windows show the inspector in a sheet rather than docked.
  inline constexpr CGFloat kDockedInspectorMinimumWidth = 1000;
} // namespace ao::appkit
