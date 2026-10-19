// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace ao::i18n
{
  class MessageCatalog;
}

namespace ao::rt
{
  class ConfigStore;
  class NotificationService;
}

namespace ao::tui
{
  inline constexpr auto kPreferencesConfigGroup = std::string_view{"preferences"};

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
    std::string terminalTitleFormat = R"($artist " - " $title)";
    bool terminalTitleSoul = true;
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

  /**
   * @brief Reports the configuration the startup loads could not use, once each of them has read its group.
   *
   * A newer `shortcuts`, `runtime`, or `preferences` group posts its own
   * newer-version warning. Preferences that could not be read for any other
   * reason post the load-failure warning with the reason.
   */
  void reportConfigLoadFailures(rt::NotificationService& notifications,
                                i18n::MessageCatalog const& catalog,
                                rt::ConfigStore const& store,
                                Result<Preferences> const& preferencesRes);
} // namespace ao::tui
