// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace ao::rt
{
  enum class ThemePresetId : std::uint8_t
  {
    Classic,
    Modern,
  };

  inline constexpr std::string_view kClassicThemePresetId = "classic";
  inline constexpr std::string_view kModernThemePresetId = "modern";

  constexpr std::string_view themePresetToString(ThemePresetId const preset) noexcept
  {
    switch (preset)
    {
      case ThemePresetId::Modern: return kModernThemePresetId;
      case ThemePresetId::Classic:
      default: return kClassicThemePresetId;
    }
  }

  constexpr ThemePresetId themePresetFromString(std::string_view const presetId) noexcept
  {
    if (presetId == kModernThemePresetId)
    {
      return ThemePresetId::Modern;
    }

    return ThemePresetId::Classic;
  }

  struct AppPrefsState final
  {
    std::string lastOutputBackendId;
    std::string lastOutputProfileId;
    std::string lastOutputDeviceId;
    std::string lastLayoutPreset;
    std::string lastThemePreset;
  };

  struct AppSessionState final
  {
    std::string lastLibraryPath;
    std::string lastOutputBackendId;
    std::string lastOutputProfileId;
    std::string lastOutputDeviceId;
  };
} // namespace ao::rt
