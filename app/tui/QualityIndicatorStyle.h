// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/Quality.h>
#include <ao/uimodel/playback/quality/AudioQualityFormatter.h>

#include <cstdint>
#include <string>

namespace ao::tui
{
  struct QualityIndicatorStyle final
  {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    std::string label{};
  };

  QualityIndicatorStyle qualityIndicatorStyle(uimodel::AudioQualityCategory category);
  QualityIndicatorStyle qualityIndicatorStyle(audio::Quality quality);
} // namespace ao::tui
