// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/app/SettingsPersistence.h>

#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/async/Executor.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/winui/DesktopSettingsYamlSchema.h>

namespace ao::winui
{
  WorkspaceSortCheckpoint::WorkspaceSortCheckpoint(async::Executor& executor,
                                                   rt::WorkspaceService& workspace,
                                                   rt::ConfigStore& store)
    : _executor{executor}, _workspace{workspace}, _store{store}
  {
  }

  void WorkspaceSortCheckpoint::request()
  {
    AO_EXPECTS(_executor.isCurrent() && _callbackGate.isOpen());

    if (_pending)
    {
      return;
    }

    _executor.defer(
      [this, token = _callbackGate.token()]
      {
        if (token.accepts())
        {
          _pending = false;
          _workspace.saveSession(_store);
        }
      });
    _pending = true;
  }

  void WorkspaceSortCheckpoint::retire() noexcept
  {
    AO_EXPECTS(_executor.isCurrent());
    _callbackGate.retire();
  }

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
    return settingsStore.save(kDesktopSettingsConfigGroup, settings, DesktopSettingsYamlSchema{});
  }
} // namespace ao::winui
