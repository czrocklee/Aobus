// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <filesystem>

namespace ao::utility
{
  /**
   * @brief The directory holding this application's per-user configuration.
   *
   * Resolves the operating system's per-user configuration location and appends
   * the application directory in the form each platform expects: `aobus` under
   * `XDG_CONFIG_HOME`, `$HOME/.config`, or the account's home directory on
   * POSIX; `Aobus` under `LOCALAPPDATA` or `APPDATA` on Windows.
   *
   * Every frontend that keeps application-global state resolves it here, so the
   * platform rule and the directory name are decided once rather than at each
   * composition root. The directory is not created and is not guaranteed to
   * exist; callers create what they need.
   *
   * Every candidate is required to be absolute. A relative value resolves
   * against the working directory, so honoring one would scatter configuration
   * wherever the process happened to be launched from; such a value is treated
   * as unset and the next candidate answers.
   *
   * Returns `NotFound` only when nothing names a home or profile location at
   * all. What a frontend does then depends on what it keeps here:
   *
   * - A frontend keeping only preferences degrades rather than failing, because
   *   losing the whole session is a heavier answer than losing preferences that
   *   were not going to survive it. `rt::ConfigStore::NoLocation` names that
   *   state, so no caller has to invent a path or spread a null check. GTK and
   *   the TUI work this way.
   * - A frontend using this as a required state root cannot: Windows puts its
   *   settings, its playback state, and the fallback library it opens when no
   *   other root is selected under this directory, so without one it has no
   *   library to show and reports a startup failure instead.
   */
  Result<std::filesystem::path> applicationConfigDirectory();
} // namespace ao::utility
