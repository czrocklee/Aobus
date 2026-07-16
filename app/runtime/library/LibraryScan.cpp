// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ScanPlanBuilder.h"
#include <ao/Error.h>
#include <ao/rt/library/LibraryScan.h>
#include <ao/rt/library/ScanPlan.h>

#include <utility>

namespace ao::rt
{
  LibraryScan::LibraryScan(library::MusicLibrary const& library)
    : _library{library}
  {
  }

  Result<ScanPlan> LibraryScan::buildPlan(BuildProgressCallback progress)
  {
    auto builder = ScanPlanBuilder{_library};
    return builder.buildPlan(std::move(progress));
  }
} // namespace ao::rt
