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

  /**
   * @brief The directory holding this application's per-user derived caches.
   *
   * Resolves the operating system's per-user cache location the same way as
   * applicationConfigDirectory() resolves configuration: `aobus` under
   * `XDG_CACHE_HOME`, `$HOME/.cache`, or the account's home directory on POSIX;
   * `Aobus\Cache` under `LOCALAPPDATA` on Windows, which has no separate cache
   * convention of its own. The same absolute-path requirement applies, for the
   * same reason.
   *
   * This is a shared facility rather than a private helper because it has a
   * caller in every composition root from the day it lands. The runtime owns
   * paths derived from a supplied root and does not discover platform
   * application directories, so each frontend resolves this and passes it down
   * the channel that already carries the music root and the database path.
   *
   * Returns `NotFound` only when nothing names a home or profile location. No
   * frontend fails to start over that: what lives here is derived, so a
   * composition root that cannot resolve it supplies nothing and the cover-read
   * walk has one tier instead of two.
   */
  Result<std::filesystem::path> applicationCacheDirectory();
} // namespace ao::utility
