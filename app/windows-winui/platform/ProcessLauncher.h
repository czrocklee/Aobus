// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/winui/app/StartupOptions.h>

#include <filesystem>

namespace ao::winui
{
  /// Parse the unpackaged process's real Win32 command line, excluding argv[0].
  Result<StartupOptions> readStartupOptions();

  /// Launch this exact executable as the sole owner of @p libraryRoot.
  Result<> launchLibraryProcess(std::filesystem::path const& libraryRoot);
} // namespace ao::winui
