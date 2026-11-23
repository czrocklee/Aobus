// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <filesystem>

namespace ao::utility
{
  // False for a filename unavailable on this host; an error for disk path
  // syntax (such as a Windows stream) instead of a literal filename.
  Result<bool> isRepresentableFileName(std::filesystem::path const& component);

  // Validate spelling without consulting the filesystem or collapsing dot
  // components. Windows accepts ordinary relative names and complete disk
  // roots only, projecting admitted verbatim paths back to ordinary spelling.
  Result<std::filesystem::path> checkedFileSpelling(std::filesystem::path const& path);

  // A disk I/O spelling, not a URI, database location, or display path. Windows
  // requires fully qualified, unambiguous DOS/UNC names in the verbatim namespace.
  Result<std::filesystem::path> fileAccessPath(std::filesystem::path const& path);

  // Project an admitted disk path (including a canonical result) back into
  // the common spelling for root comparisons, plan provenance, and display.
  std::filesystem::path ordinaryFilePath(std::filesystem::path const& path);

  // Resolve the existing prefix through the I/O spelling, returning ordinary
  // path components for comparison even if the suffix does not yet exist.
  Result<std::filesystem::path> weaklyCanonicalFilePath(std::filesystem::path const& path);
} // namespace ao::utility
