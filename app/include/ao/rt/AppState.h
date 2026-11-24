// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/OutputDeviceSelection.h>

#include <string>
#include <string_view>

namespace ao::rt
{
  class ConfigStore;

  struct AppPrefsState final
  {
    audio::OutputDeviceSelection preferredOutputSelection{};
    std::string lastLayoutPreset;
    std::string lastThemePreset;
  };

  struct AppSessionState final
  {
    std::string lastLibraryPath;
    audio::OutputDeviceSelection lastOutputSelection{};
  };

  /**
   * @name Application-global configuration groups
   *
   * These group names and shapes are part of the on-disk contract for frontends
   * that use these application-global configuration groups. Each such frontend
   * chooses the file in which they live.
   * @{
   */
  inline constexpr std::string_view kAppPrefsConfigGroup = "runtime";
  inline constexpr std::string_view kAppSessionConfigGroup = "session";
  /// @}

  /**
   * @brief Reads persisted application preferences over @p state.
   *
   * Route identity and preset fields retain the caller's defaults when absent
   * or when the group is rejected. Rerouting permission is never seeded: only
   * a valid opt-in for a complete stored eligible route enables it. Invalid
   * permission values leave the route fixed rather than rejecting its identity.
   */
  void loadAppPrefs(ConfigStore& store, AppPrefsState& state);
  Result<> saveAppPrefs(ConfigStore& store, AppPrefsState const& state);

  /// Reads persisted session fields over @p state; the last active route never restores rerouting permission.
  void loadAppSession(ConfigStore& store, AppSessionState& state);
  Result<> saveAppSession(ConfigStore& store, AppSessionState const& state);
} // namespace ao::rt
