// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/app/SettingsPersistence.h>

#include <ao/Error.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/winui/DesktopSettingsYamlSchema.h>

namespace ao::winui
{
  Result<> persistDesktopSettingsCandidate(rt::WorkspaceService& workspace,
                                           rt::ConfigStore& workspaceStore,
                                           rt::ConfigStore& settingsStore,
                                           DesktopSettings const& settings)
  {
    // Best-effort checkpoint: WorkspaceService::saveSession logs its own
    // storage failure, which is diagnostic-only for this composite.
    workspace.saveSession(workspaceStore);

    // The checked desktop save is the composite's result; it does not undo the
    // workspace checkpoint that already ran.
    return settingsStore.save("desktop", settings, DesktopSettingsYamlSchema{});
  }
} // namespace ao::winui
