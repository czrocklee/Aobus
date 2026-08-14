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

  /** Resolve preferred intent or a complete last-active fallback without issuing a command. */
  std::optional<audio::OutputDeviceSelection> resolveOutputDeviceSelectionToRestore(
    audio::OutputDeviceSelection const& preferred,
    audio::OutputDeviceSelection const& fallback,
    rt::OutputState const& output);
} // namespace ao::uimodel
