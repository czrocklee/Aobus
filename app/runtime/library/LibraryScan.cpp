// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ScanPlanApplier.h"
#include "ScanPlanBuilder.h"
#include <ao/Error.h>
#include <ao/rt/library/LibraryScan.h>
#include <ao/rt/library/ScanPlan.h>

#include <stop_token>
#include <utility>

namespace ao::rt
{
  LibraryScan::LibraryScan(library::MusicLibrary& library)
    : _library{library}
  {
  }

  Result<ScanPlan> LibraryScan::buildPlan(BuildProgressCallback progress)
  {
    auto builder = ScanPlanBuilder{_library};
    return builder.buildPlan(std::move(progress));
  }

  Result<ScanApplyResult> LibraryScan::applyPlan(ScanPlan plan,
                                                 ScanApplyOptions options,
                                                 ApplyProgressCallback progress,
                                                 FailureCallback failure,
                                                 std::stop_token stopToken)
  {
    auto applier = ScanPlanApplier{_library, std::move(plan), std::move(progress), std::move(failure), options};
    return applier.run(stopToken);
  }
} // namespace ao::rt
