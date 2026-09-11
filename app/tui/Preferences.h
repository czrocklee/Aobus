// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstdint>
#include <string>

namespace ao::rt
{
  class ConfigStore;
}

namespace ao::tui
{
  inline constexpr std::int32_t kMaximumSeekSeconds = 60;
  inline constexpr std::int32_t kMaximumWheelStep = 10;
  inline constexpr std::int32_t kMaximumVolumePercent = 10;
  inline constexpr std::int32_t kDefaultSeekSeconds = 5;
  inline constexpr std::int32_t kDefaultVolumePercent = 5;

  struct Preferences final
  {
    // Empty means follow the system, rather than pinning its current locale.
    std::string language{};
    std::string coverArtMode = "auto";
    std::string panelSeparator = "single";
    bool revealIndicatorsOnHover = false;
    bool dimBackdrop = true;
    bool reducedMotion = false;
    bool mouseEnabled = true;
    bool qualityHover = true;
    std::int32_t wheelStep = 3;
    std::int32_t seekSeconds = kDefaultSeekSeconds;
    std::int32_t volumePercent = kDefaultVolumePercent;

    bool operator==(Preferences const&) const = default;
  };

  Result<Preferences> loadPreferences(rt::ConfigStore& store);
  Result<> savePreferences(rt::ConfigStore& store, Preferences const& preferences);
} // namespace ao::tui
