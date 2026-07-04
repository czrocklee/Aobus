// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ScanOutput.h"

#include <ao/library/LibraryScanner.h>

#include <string_view>

namespace ao::cli
{
  std::string_view scanClassificationName(library::ScanClassification classification)
  {
    switch (classification)
    {
      case library::ScanClassification::New: return "new";
      case library::ScanClassification::Changed: return "changed";
      case library::ScanClassification::Moved: return "moved";
      case library::ScanClassification::Missing: return "missing";
      case library::ScanClassification::Unchanged: return "unchanged";
      case library::ScanClassification::Error: return "error";
    }

    return "unknown";
  }
} // namespace ao::cli
