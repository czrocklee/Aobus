// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/playback/quality/AudioQualityFormatter.h>

#include <cstdint>

namespace ao::tui
{
  struct QualityIndicatorStyle final
  {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
  };

  QualityIndicatorStyle qualityIndicatorStyle(uimodel::AudioQualityCategory category);
} // namespace ao::tui
