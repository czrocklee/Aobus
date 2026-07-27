// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/rt/library/ScanPlan.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace ao::rt
{
  class LibraryTaskService;
}

namespace ao::uimodel
{
  enum class LibraryScanMode : std::uint8_t
  {
    Eager,
    FastBootstrap
  };

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

  struct LibraryScanPlanSummary final
  {
    std::size_t newCount = 0;
    std::size_t changedCount = 0;
    std::size_t movedCount = 0;
    std::size_t missingCount = 0;
    std::size_t errorCount = 0;

    friend bool operator==(LibraryScanPlanSummary const&, LibraryScanPlanSummary const&) = default;
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
  async::Task<std::expected<LibraryScanWorkflowResult, LibraryScanWorkflowFailure>>
  runLibraryScanWorkflow(rt::LibraryTaskService* service, LibraryScanMode mode, std::stop_token stopToken = {});
} // namespace ao::uimodel
