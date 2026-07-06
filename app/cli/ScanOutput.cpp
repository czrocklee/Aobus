// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ScanOutput.h"

#include <ao/rt/library/ScanPlan.h>

#include <string_view>

namespace ao::cli
{
  std::string_view scanClassificationName(rt::ScanClassification classification)
  {
    switch (classification)
    {
      case rt::ScanClassification::New: return "new";
      case rt::ScanClassification::Changed: return "changed";
      case rt::ScanClassification::Moved: return "moved";
      case rt::ScanClassification::Missing: return "missing";
      case rt::ScanClassification::Unchanged: return "unchanged";
      case rt::ScanClassification::Error: return "error";
    }

    return "unknown";
  }
} // namespace ao::cli
