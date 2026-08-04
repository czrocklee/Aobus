// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>
#include <ao/winui/layout/ShellDocument.h>

#include <string>

namespace ao::winui::layout
{
  /**
   * @brief Read the YAML of a built-in preset from the packaged layout folder.
   *
   * The documents ship as application content rather than as compiled-in
   * strings so that the shell a build produces is the one on disk: a developer
   * editing a preset restarts the app rather than the build, and a malformed
   * edit is reported the same way a user document would be.
   */
  Result<std::string> readShellPreset(ShellPreset preset);

  /**
   * @brief Read a built-in preset and validate it as one candidate.
   *
   * A shipped document that does not prepare is a build defect, so the error
   * names the resource rather than degrading to a partial shell.
   */
  Result<uimodel::PreparedLayout> prepareShellPreset(ShellPreset preset);
} // namespace ao::winui::layout
