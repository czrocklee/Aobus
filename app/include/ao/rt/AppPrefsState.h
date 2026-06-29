// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <string>

namespace ao::rt
{
  enum class ThemePresetId : std::uint8_t
  {
    Classic,
    Modern,
  };

  struct AppPrefsState final
  {
    std::string lastLibraryPath;
    std::string lastOutputBackendId;
    std::string lastOutputProfileId;
    std::string lastOutputDeviceId;
    std::string lastLayoutPreset;
    std::string lastThemePreset;
  };
} // namespace ao::rt
