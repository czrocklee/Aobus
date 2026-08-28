// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/playback/output/OutputSelection.h>

#include <ao/audio/BackendIds.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/rt/PlaybackState.h>

#include <algorithm>
#include <optional>

namespace ao::uimodel
{
  namespace
  {
    rt::OutputBackendSnapshot const* findOutputBackend(rt::OutputState const& output,
                                                       audio::BackendId const& backendId) noexcept
    {
      auto const it = std::ranges::find(output.availableBackends, backendId, &rt::OutputBackendSnapshot::id);
      return it == output.availableBackends.end() ? nullptr : &*it;
    }

    bool canRestoreWithPublishedBackend(audio::OutputDeviceSelection const& selection,
                                        rt::OutputBackendSnapshot const& backend) noexcept
    {
      if (!std::ranges::contains(backend.supportedProfiles, selection.profileId, &rt::OutputProfileSnapshot::id))
      {
        return false;
      }

      if (!selection.deviceId.empty())
      {
        return true;
      }

      return std::ranges::any_of(
        backend.devices,
        [&selection](rt::OutputDeviceSnapshot const& device)
        { return device.id.empty() && device.isDefault && rt::supportsOutputProfile(device, selection.profileId); });
    }
  } // namespace

  bool canRestoreOutputDeviceSelection(audio::OutputDeviceSelection const& selection,
                                       rt::OutputState const& output) noexcept
  {
    if (selection.backendId.empty() || selection.profileId.empty())
    {
      return false;
    }

    auto const* const backend = findOutputBackend(output, selection.backendId);

    if (backend == nullptr)
    {
      return !selection.deviceId.empty();
    }

    return canRestoreWithPublishedBackend(selection, *backend);
  }

  std::optional<audio::OutputDeviceSelection> resolveOutputDeviceSelectionToRestore(
    audio::OutputDeviceSelection const& preferred,
    audio::OutputDeviceSelection const& fallback,
    rt::OutputState const& output)
  {
    if (canRestoreOutputDeviceSelection(preferred, output))
    {
      return preferred;
    }

    return canRestoreOutputDeviceSelection(fallback, output) ? std::optional<audio::OutputDeviceSelection>{fallback}
                                                             : std::nullopt;
  }
} // namespace ao::uimodel
