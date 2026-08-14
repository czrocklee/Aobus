// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/audio/OutputDeviceSelection.h>

#include <optional>

namespace ao::rt
{
  struct OutputState;
}

namespace ao::winui
{
  struct DesktopSettings;

  /** Resolve the Windows desktop preference against the currently published output catalog. */
  std::optional<audio::OutputDeviceSelection> resolveDesktopOutputSelectionToRestore(DesktopSettings const& settings,
                                                                                     rt::OutputState const& output);

  /** Update the in-memory desktop preference and report whether it changed. */
  bool rememberDesktopOutputSelection(DesktopSettings& settings,
                                      audio::OutputDeviceSelection const& selection) noexcept;
} // namespace ao::winui
