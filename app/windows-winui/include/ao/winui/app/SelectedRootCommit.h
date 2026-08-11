// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/winui/DesktopSettingsYamlSchema.h>

#include <filesystem>

namespace ao::winui
{
  /** Prepare a desktop-settings candidate without mutating the live settings snapshot. */
  Result<DesktopSettings> prepareSelectedRootCommit(DesktopSettings const& settings, std::filesystem::path const& root);
} // namespace ao::winui
