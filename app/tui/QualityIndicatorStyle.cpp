// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "QualityIndicatorStyle.h"

#include <ao/uimodel/playback/quality/AudioQualityFormatter.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

namespace ao::tui
{
  namespace
  {
    void applyIndicatorColor(QualityIndicatorStyle& style, uimodel::AobusSoulRgb const color)
    {
      style.red = color.red;
      style.green = color.green;
      style.blue = color.blue;
    }
  } // namespace

  QualityIndicatorStyle qualityIndicatorStyle(uimodel::AudioQualityCategory const category)
  {
    auto style = QualityIndicatorStyle{};

    switch (category)
    {
      case uimodel::AudioQualityCategory::Medal: applyIndicatorColor(style, uimodel::kAobusSoulRadiant); return style;
      case uimodel::AudioQualityCategory::Positive:
        applyIndicatorColor(style, uimodel::kAobusSoulFlowing);
        return style;
      case uimodel::AudioQualityCategory::Diagnostic:
      case uimodel::AudioQualityCategory::Warning:
        applyIndicatorColor(style, uimodel::kAobusSoulTurbulent);
        return style;
      case uimodel::AudioQualityCategory::Informational:
        applyIndicatorColor(style, uimodel::kAobusSoulVeiled);
        return style;
      case uimodel::AudioQualityCategory::Clipped: applyIndicatorColor(style, uimodel::kAobusSoulBurning); return style;
      case uimodel::AudioQualityCategory::Unknown: applyIndicatorColor(style, uimodel::kAobusSoulVeiled); return style;
    }

    applyIndicatorColor(style, uimodel::kAobusSoulVeiled);
    return style;
  }
} // namespace ao::tui
