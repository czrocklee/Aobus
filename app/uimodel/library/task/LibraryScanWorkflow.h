// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/uimodel/library/task/LibraryScanOutcome.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace ao::rt
{
  class LibraryJobs;
}

namespace ao::uimodel
{
  enum class LibraryScanPlanDisposition : std::uint8_t
  {
    UpToDate,
    ErrorsOnly,
    Actionable
  };

  enum class LibraryScanWorkflowStage : std::uint8_t
  {
    Planning,
    Applying
  };

  struct LibraryScanIssue final
  {
    std::string uri{};
    std::string message{};

    friend bool operator==(LibraryScanIssue const&, LibraryScanIssue const&) = default;
  };

  struct LibraryScanWorkflowResult final
  {
    LibraryScanPlanDisposition disposition = LibraryScanPlanDisposition::UpToDate;
    LibraryScanPlanSummary summary{};
    std::vector<LibraryScanIssue> issues{};
    std::optional<rt::ScanApplyResult> optApplyResult{};
    bool shouldBackfillAudioIdentity = false;

    bool mutatedLibrary() const noexcept;
  };

  struct LibraryScanWorkflowFailure final
  {
    LibraryScanWorkflowStage stage = LibraryScanWorkflowStage::Planning;
    Error error{};
    std::optional<LibraryScanPlanSummary> optPlanSummary{};
  };

  LibraryScanPlanDisposition decideLibraryScanPlan(LibraryScanPlanSummary const& summary) noexcept;

  /**
   * Also writes the plan summary and any failure to the log, and the unreadable
   * files when nothing was applied. Reading a scan and recording what it saw is
   * one pass over the same data, and splitting them is how one shell ends up
   * with diagnostics the other silently lacks. An applied plan reports its own
   * failed items as it reaches them, so those are not repeated here.
   */
  LibraryScanOutcome decideLibraryScanOutcome(
    std::expected<LibraryScanWorkflowResult, LibraryScanWorkflowFailure> const& result);
  async::Task<std::expected<LibraryScanWorkflowResult, LibraryScanWorkflowFailure>>
  runLibraryScanWorkflow(rt::LibraryJobs* jobs, LibraryScanMode mode, std::stop_token stopToken = {});
} // namespace ao::uimodel
