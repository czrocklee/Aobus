// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/app/DesktopOutputSelection.h>

#include <ao/audio/OutputDeviceSelection.h>
#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/playback/output/OutputDeviceSelectionPolicy.h>
#include <ao/winui/DesktopSettingsYamlSchema.h>

#include <optional>

namespace ao::winui
{
  std::optional<audio::OutputDeviceSelection> resolveDesktopOutputSelectionToRestore(DesktopSettings const& settings,
                                                                                     rt::OutputState const& output)
  {
    return uimodel::resolveOutputDeviceSelectionToRestore(settings.preferredOutputSelection, {}, output);
  }

  bool rememberDesktopOutputSelection(DesktopSettings& settings, audio::OutputDeviceSelection const& selection) noexcept
  {
    if (settings.preferredOutputSelection == selection)
    {
      return false;
    }

    settings.preferredOutputSelection = selection;
    return true;
  }
} // namespace ao::winui
