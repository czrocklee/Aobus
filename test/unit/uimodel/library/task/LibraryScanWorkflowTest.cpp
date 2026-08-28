// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/uimodel/library/task/LibraryScanWorkflow.h"

#include <ao/rt/library/ScanPlan.h>
#include <ao/uimodel/library/task/LibraryScanOutcome.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::test
{
  TEST_CASE("LibraryScanWorkflow - plan decision distinguishes success, rejection, and work",
            "[uimodel][unit][library][scan]")
  {
    CHECK(decideLibraryScanPlan({}) == LibraryScanPlanDisposition::UpToDate);
    auto summary = LibraryScanPlanSummary{};
    summary.errorCount = 2;
    CHECK(decideLibraryScanPlan(summary) == LibraryScanPlanDisposition::ErrorsOnly);
    summary.newCount = 1;
    CHECK(decideLibraryScanPlan(summary) == LibraryScanPlanDisposition::Actionable);
    summary = {};
    summary.changedCount = 1;
    CHECK(decideLibraryScanPlan(summary) == LibraryScanPlanDisposition::Actionable);
    summary = {};
    summary.movedCount = 1;
    CHECK(decideLibraryScanPlan(summary) == LibraryScanPlanDisposition::Actionable);
    summary = {};
    summary.missingCount = 1;
    CHECK(decideLibraryScanPlan(summary) == LibraryScanPlanDisposition::Actionable);
  }

  TEST_CASE("LibraryScanWorkflow - mutation detection excludes issue-only results", "[uimodel][unit][library][scan]")
  {
    auto result = LibraryScanWorkflowResult{};
    CHECK_FALSE(result.mutatedLibrary());

    result.optApplyResult = rt::ScanApplyResult{};
    result.optApplyResult->missingCount = 1;
    CHECK_FALSE(result.mutatedLibrary());

    result.optApplyResult->insertedIds.emplace_back(1);
    CHECK(result.mutatedLibrary());
  }
} // namespace ao::uimodel::test
