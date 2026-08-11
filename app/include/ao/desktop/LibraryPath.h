// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <filesystem>

namespace ao::desktop
{
  /** Resolve a non-empty library root to an absolute lexical path. */
  Result<std::filesystem::path> normalizeLibraryRoot(std::filesystem::path root);

  /** Resolve a library root and require that it names an accessible directory. */
  Result<std::filesystem::path> normalizeExistingLibraryRoot(std::filesystem::path root);

  /** Compare normalized roots by filesystem identity with a lexical fallback. */
  bool sameLibraryRoot(std::filesystem::path const& left, std::filesystem::path const& right);
} // namespace ao::desktop
