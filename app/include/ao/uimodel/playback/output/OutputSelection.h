// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/audio/OutputDeviceSelection.h>
#include <ao/rt/PlaybackState.h>

#include <optional>

namespace ao::uimodel
{
  /** Whether persisted intent can be submitted without contradicting the currently published catalog. */
  bool canRestoreOutputDeviceSelection(audio::OutputDeviceSelection const& selection,
                                       rt::OutputState const& output) noexcept;

  /** Preserve an explicit endpoint (or a supported follow-system route) before considering a last-active fallback. */
  std::optional<audio::OutputDeviceSelection> resolveOutputDeviceSelectionToRestore(
    audio::OutputDeviceSelection const& preferred,
    audio::OutputDeviceSelection const& fallback,
    rt::OutputState const& output);
} // namespace ao::uimodel
