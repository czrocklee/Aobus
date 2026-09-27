// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

namespace ao::rt
{
  class ConfigStore;
  class WorkspaceService;
}

namespace ao::winui
{
  struct DesktopSettings;

  /**
   * @brief Persists a desktop-settings candidate through the two-store sequence.
   *
   * The workspace checkpoint runs first and is deliberately best-effort:
   * WorkspaceService::saveSession reports its own storage failure and returns
   * void, and that failure is diagnostic-only here. It never fails or blocks
   * the desktop-settings save, whose Result alone is this operation's result
   * and may alone gate durable library-root admission.
   *
   * Neither write rolls back the other: a workspace checkpoint that ran before
   * a failed desktop save stays written, and a failed desktop save leaves the
   * settings store unchanged.
   */
  Result<> persistDesktopSettingsCandidate(rt::WorkspaceService& workspace,
                                           rt::ConfigStore& workspaceStore,
                                           rt::ConfigStore& settingsStore,
                                           DesktopSettings const& settings);
} // namespace ao::winui
