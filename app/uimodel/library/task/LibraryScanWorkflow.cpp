// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/task/LibraryScanWorkflow.h>

#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/rt/library/LibraryTaskService.h>
#include <ao/rt/library/ScanPlan.h>

#include <expected>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  namespace
  {
    LibraryScanPlanSummary summarize(rt::ScanPlan const& plan) noexcept
    {
      return {
        .newCount = plan.count(rt::ScanClassification::New),
        .changedCount = plan.count(rt::ScanClassification::Changed),
        .movedCount = plan.count(rt::ScanClassification::Moved),
        .missingCount = plan.count(rt::ScanClassification::Missing),
        .errorCount = plan.count(rt::ScanClassification::Error),
      };
    }

    std::vector<LibraryScanIssue> collectIssues(rt::ScanPlan const& plan)
    {
      auto issues = std::vector<LibraryScanIssue>{};
      issues.reserve(plan.count(rt::ScanClassification::Error));

      for (auto const& item : plan.items())
      {
        if (item.classification == rt::ScanClassification::Error)
        {
          issues.push_back({.uri = item.uri, .message = item.errorMessage});
        }
      }

      return issues;
    }
  } // namespace

  bool LibraryScanWorkflowResult::mutatedLibrary() const noexcept
  {
    return optApplyResult && (!optApplyResult->insertedIds.empty() || !optApplyResult->mutatedIds.empty() ||
                              !optApplyResult->relinkedIds.empty());
  }

  LibraryScanPlanDisposition decideLibraryScanPlan(LibraryScanPlanSummary const& summary) noexcept
  {
    if (summary.newCount != 0 || summary.changedCount != 0 || summary.movedCount != 0 || summary.missingCount != 0)
    {
      return LibraryScanPlanDisposition::Actionable;
    }

    return summary.errorCount == 0 ? LibraryScanPlanDisposition::UpToDate : LibraryScanPlanDisposition::ErrorsOnly;
  }

  async::Task<std::expected<LibraryScanWorkflowResult, LibraryScanWorkflowFailure>> runLibraryScanWorkflow(
    rt::LibraryTaskService* const service,
    LibraryScanMode const mode,
    std::stop_token const stopToken)
  {
    auto planRes = co_await service->buildScanPlanAsync(stopToken);

    if (!planRes)
    {
      co_return std::unexpected{LibraryScanWorkflowFailure{
        .stage = LibraryScanWorkflowStage::Planning,
        .error = std::move(planRes.error()),
      }};
    }

    auto plan = std::move(*planRes);
    auto result = LibraryScanWorkflowResult{};
    result.summary = summarize(plan);
    result.disposition = decideLibraryScanPlan(result.summary);
    result.issues = collectIssues(plan);

    if (result.disposition != LibraryScanPlanDisposition::Actionable)
    {
      co_return result;
    }

    auto options = rt::ScanApplyOptions{};

    if (mode == LibraryScanMode::FastBootstrap)
    {
      options.audioIdentityPolicy = rt::AudioIdentityPolicy::DeferNew;
    }

    auto appliedRes = co_await service->applyScanPlanAsync(std::move(plan), options, stopToken);

    if (!appliedRes)
    {
      co_return std::unexpected{LibraryScanWorkflowFailure{
        .stage = LibraryScanWorkflowStage::Applying,
        .error = std::move(appliedRes.error()),
        .optPlanSummary = result.summary,
      }};
    }

    result.optApplyResult = std::move(*appliedRes);
    result.shouldBackfillAudioIdentity = mode == LibraryScanMode::FastBootstrap;
    co_return result;
  }
} // namespace ao::uimodel
