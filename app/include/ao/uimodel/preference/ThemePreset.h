// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <string_view>

namespace ao::uimodel
{
  enum class ThemePreset : std::uint8_t
  {
    Classic,
    Modern,
  };

  inline constexpr std::string_view kClassicThemePresetId = "classic";
  inline constexpr std::string_view kModernThemePresetId = "modern";

  constexpr std::string_view themePresetId(ThemePreset const preset) noexcept
  {
    switch (preset)
    {
      case ThemePreset::Modern: return kModernThemePresetId;
      case ThemePreset::Classic:
      default: return kClassicThemePresetId;
    }
  }

  constexpr ThemePreset themePresetFromId(std::string_view const presetId) noexcept
  {
    if (presetId == kModernThemePresetId)
    {
      return ThemePreset::Modern;
    }

    return ThemePreset::Classic;
  }
} // namespace ao::uimodel
