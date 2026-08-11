// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/desktop/LibrarySwitch.h>

#include <filesystem>
#include <optional>

namespace ao::winui
{
  /// Parse the unpackaged process's real Win32 command line, excluding argv[0].
  Result<std::optional<desktop::LibrarySwitchRequest>> readLibrarySuccessorRequest();

  /// Launch this exact executable as the sole owner of @p request.
  Result<> launchLibraryProcess(desktop::LibrarySwitchRequest const& request);
} // namespace ao::winui
