// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/library/task/LibraryScanOutcome.h>

#include "LibraryScanWorkflow.h"
#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/rt/Log.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/library/ScanPlan.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <stop_token>
#include <string_view>
#include <vector>

namespace ao::uimodel
{
  namespace
  {
    constexpr std::string_view stageName(LibraryScanWorkflowStage const stage) noexcept
    {
      switch (stage)
      {
        case LibraryScanWorkflowStage::Planning: return "planning";
        case LibraryScanWorkflowStage::Applying: return "applying";
      }

      return "scanning";
    }

    void logPlan(LibraryScanPlanSummary const& summary)
    {
      APP_LOG_INFO("Scan plan: {} new, {} changed, {} moved, {} missing, {} errors, {} notices",
                   summary.newCount,
                   summary.changedCount,
                   summary.movedCount,
                   summary.missingCount,
                   summary.errorCount,
                   summary.noticeCount);
    }

    // No apply pass reports a notice, so this pass logs every one. Failures are
    // logged here only when @p logsFailures, because an applied plan reports
    // its own failed items.
    void logIssues(std::vector<LibraryScanIssue> const& issues, bool const logsFailures)
    {
      for (auto const& issue : issues)
      {
        if (issue.isNotice)
        {
          APP_LOG_WARN("Scan notice for {}: {}", issue.uri, issue.message);
        }
        else if (logsFailures)
        {
          APP_LOG_ERROR("Failed to scan {}: {}", issue.uri, issue.message);
        }
      }
    }

    std::size_t nonNegative(std::int32_t const count) noexcept
    {
      return static_cast<std::size_t>(std::max(count, 0));
    }
  } // namespace

  rt::NotificationSeverity libraryScanSeverity(LibraryScanVerdict const verdict) noexcept
  {
    switch (verdict)
    {
      case LibraryScanVerdict::UpToDate:
      case LibraryScanVerdict::Complete: return rt::NotificationSeverity::Info;
      case LibraryScanVerdict::NeedsReview:
      case LibraryScanVerdict::CompletedWithErrors: return rt::NotificationSeverity::Warning;
      case LibraryScanVerdict::Unreadable:
      case LibraryScanVerdict::Failed: return rt::NotificationSeverity::Error;
    }

    return rt::NotificationSeverity::Error;
  }

  rt::NotificationLifetime libraryScanLifetime(LibraryScanVerdict const verdict) noexcept
  {
    return libraryScanSeverity(verdict) == rt::NotificationSeverity::Info ? rt::NotificationLifetime::transient()
                                                                          : rt::NotificationLifetime::history();
  }

  LibraryScanOutcome decideLibraryScanOutcome(
    std::expected<LibraryScanWorkflowResult, LibraryScanWorkflowFailure> const& result)
  {
    if (!result)
    {
      auto const& failure = result.error();

      if (failure.optPlanSummary)
      {
        logPlan(*failure.optPlanSummary);
      }

      APP_LOG_ERROR("Library scan failed while {}: code={}, message={}, location={}:{}",
                    stageName(failure.stage),
                    static_cast<int>(failure.error.code),
                    failure.error.message,
                    failure.error.location.file_name(),
                    failure.error.location.line());

      return {
        .verdict = LibraryScanVerdict::Failed,
        .summary = failure.optPlanSummary.value_or(LibraryScanPlanSummary{}),
        .optError = failure.error,
      };
    }

    auto const& completed = *result;
    logPlan(completed.summary);

    auto outcome = LibraryScanOutcome{
      .summary = completed.summary,
      .shouldBackfillAudioIdentity = completed.shouldBackfillAudioIdentity,
      .issues = completed.issues,
    };

    // Nothing was applied on an unreadable plan, so nothing else reported its
    // failures. On an actionable plan the apply operation reports each failed
    // item as it reaches it, and logging them again here would double every entry.
    logIssues(completed.issues, completed.disposition == LibraryScanPlanDisposition::ErrorsOnly);
    auto const hasNotices = completed.summary.noticeCount > 0;

    switch (completed.disposition)
    {
      case LibraryScanPlanDisposition::UpToDate:
        outcome.verdict = hasNotices ? LibraryScanVerdict::NeedsReview : LibraryScanVerdict::UpToDate;
        return outcome;
      case LibraryScanPlanDisposition::ErrorsOnly:
        outcome.verdict = LibraryScanVerdict::Unreadable;
        outcome.failureCount = completed.summary.errorCount;
        return outcome;
      case LibraryScanPlanDisposition::Actionable: break;
    }

    if (!completed.optApplyResult)
    {
      // The workflow reports an applied plan or a failure; an actionable plan
      // with neither means the two disagree, which no message can paper over.
      outcome.verdict = LibraryScanVerdict::Failed;
      outcome.optError = Error{
        .code = Error::Code::InvalidState,
        .message = "The scan found changes to apply but reported no result",
      };
      return outcome;
    }

    auto const& applied = *completed.optApplyResult;
    outcome.relinkedCount = nonNegative(applied.relinkedFileCount);
    outcome.missingCount = nonNegative(applied.missingCount);
    outcome.staleCount = nonNegative(applied.staleCount);
    outcome.failureCount = nonNegative(applied.failureCount);

    if (outcome.failureCount > 0)
    {
      outcome.verdict = LibraryScanVerdict::CompletedWithErrors;
    }
    else if (outcome.missingCount > 0 || hasNotices)
    {
      outcome.verdict = LibraryScanVerdict::NeedsReview;
    }
    else
    {
      outcome.verdict = LibraryScanVerdict::Complete;
    }

    return outcome;
  }

  async::Task<LibraryScanOutcome> runLibraryScanAsync(rt::LibraryJobs* const jobs,
                                                      LibraryScanMode const mode,
                                                      std::stop_token const stopToken)
  {
    auto result = co_await runLibraryScanWorkflowAsync(jobs, mode, stopToken);
    co_return decideLibraryScanOutcome(result);
  }
} // namespace ao::uimodel
