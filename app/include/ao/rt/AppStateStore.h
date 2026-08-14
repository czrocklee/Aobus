// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <string_view>

namespace ao::rt
{
  class ConfigStore;
  struct AppPrefsState;
  struct AppSessionState;

  /**
   * @name Application-global configuration groups
   *
   * The group names are part of the on-disk contract and are shared by every
   * frontend that keeps application-global state. A frontend chooses the file
   * these groups live in; it does not choose what they are called or what shape
   * they have.
   * @{
   */
  inline constexpr std::string_view kAppPrefsConfigGroup = "runtime";
  inline constexpr std::string_view kAppSessionConfigGroup = "session";
  /// @}

  /**
   * @brief Reads persisted application preferences over @p state.
   *
   * Every field is optional over the value @p state already holds, so a
   * document written by an older build keeps the caller's defaults for whatever
   * it predates rather than resetting the whole group. A missing group leaves
   * @p state untouched; a rejected one is logged and also leaves it untouched,
   * because discarding a user's remaining preferences to punish one bad field
   * loses more than it protects.
   */
  void loadAppPrefs(ConfigStore& store, AppPrefsState& state);

  Result<> saveAppPrefs(ConfigStore& store, AppPrefsState const& state);

  /// Reads the persisted application session over @p state, under the same rule as `loadAppPrefs`.
  void loadAppSession(ConfigStore& store, AppSessionState& state);

  Result<> saveAppSession(ConfigStore& store, AppSessionState const& state);
} // namespace ao::rt
